#include "FlawManager.hh"
#include "SolverUtils.hh"
#include "Utils.hh"
#include "FlawFilter.hh"
#include "FlawHandler.hh"
#include "Token.hh"
#include "Debug.hh"
#include "ConstraintEngineListener.hh"

#include <boost/smart_ptr/make_shared.hpp>

/**
 * @file FlawManager.cc
 * @author Conor McGann
 * @date April, 2005
 * @brief Provides implementation for FlawManager
 */
#if 0
#ifdef _MSC_VER
using stdext::hash_map;
#elif defined(__clang__)
typedef HASH_NS::unordered_map hash_map;
#else
using HASH_NS::hash_map;
#endif //_MSC_VER
#endif //0

namespace EUROPA {
namespace SOLVERS {

class FlawManager::Listener : public ConstraintEngineListener {
 public:
  //TODO: investigate why this can't be a reference
  Listener(FlawManager* flawManager) :
      ConstraintEngineListener(flawManager->getPlanDatabase()->getConstraintEngine()),
      m_flawManager(flawManager) {}
  void notifyChanged(const ConstrainedVariableId variable,
                     const DomainListener::ChangeType&) {
    m_flawManager->updateGuards(*variable);
  }
 private:
  FlawManager* m_flawManager;
};

  
FlawManager::FlawManager(const TiXmlElement& configData)
    : Component(configData) 
    , m_db()
    , m_parent()
    , m_flawFilters()
    , m_flawHandlers()
    , m_staticFiltersByKey()
    , m_dynamicFiltersByKey()
    , m_flawHandlerGuards()
    , m_activeFlawHandlersByKey()
    , m_timestamp(0)
    , m_context()
    , m_ceListener()
{
}

    FlawManager::~FlawManager()
    {
      if (!m_flawFilters.isNoId())
        delete static_cast<MatchingEngine*>(m_flawFilters);
      if (!m_flawHandlers.isNoId())
        delete static_cast<MatchingEngine*>(m_flawHandlers);
    }

  bool FlawManager::isValid() const {
    for(std::map<eint, bool>::const_iterator it = m_staticFiltersByKey.begin(); it != m_staticFiltersByKey.end(); ++it) {
      eint key = it->first;
      EntityId entity = Entity::getEntity(key);
      condDebugMsg(!entity.isValid(), "FlawManager:isValid", getId() << " Invalid id in m_staticFiltersByKey. Entity key: " << it->first);        
    }
    for( Eint2FlawFilterVectorMap::const_iterator it = m_dynamicFiltersByKey.begin(); 
         it != m_dynamicFiltersByKey.end(); 
         ++it) {
      EntityId entity = Entity::getEntity(it->first);
      condDebugMsg(!entity.isValid(), "FlawManager:isValid", getId() << " Invalid id in m_dynamicFiltersByKey. Entity key: " << it->first);
      const std::vector<FlawFilterId>& filters(it->second);
      for(std::vector<FlawFilterId>::const_iterator subIt = filters.begin(); subIt != filters.end(); ++subIt) {
        condDebugMsg(!(*subIt).isValid(), "FlawManager:isValid", getId() << " Invalid flaw filter id for entity " << it->first <<
                     " in m_dynamicFiltersByKey");
      }
    }
    for(std::multimap<eint, boost::shared_ptr<FlawHandler::VariableListener> >::const_iterator it = m_flawHandlerGuards.begin(); it != m_flawHandlerGuards.end();
        ++it) {
      EntityId entity = Entity::getEntity(it->first);
      condDebugMsg(!entity.isValid(), "FlawManager:isValid", getId() << " Invalid id in m_flawHandlerGuards.  Entity key:" << it->first);
      condDebugMsg(!it->second, "FlawManager:isValid", getId() << " Invalid constraint id in m_flawHandlerGuards.  Entity key: " << it->first);
      condDebugMsg(!it->second->getTarget().isValid(), "FlawManager:isValid", getId() << " Invalid target id in m_flawHandlerGuards.  Entity key: " << it->first);
      if(m_activeFlawHandlersByKey.find(it->first) == m_activeFlawHandlersByKey.end()) {
        debugMsg("FlawManager:isValid", getId() << "Target '" << it->second->getTarget()->toString() << "' has no active flaw handlers.");
        std::stringstream scope;
        for(std::vector<ConstrainedVariableId>::const_iterator scopeIt = it->second->scope().begin(); scopeIt != it->second->scope().end(); 
            ++scopeIt)
          scope << (*scopeIt)->toString() << " ";
        debugMsg("FlawManager:isValid", "Variable listener scope: " << scope.str());
      }
    }

    for(std::map<eint, FlawHandlerEntry>::const_iterator it = m_activeFlawHandlersByKey.begin(); it != m_activeFlawHandlersByKey.end();
        ++it) {
      EntityId entity = Entity::getEntity(it->first);
      condDebugMsg(!entity.isValid(), "FlawManager:isValid", getId() << " Invalid id in m_activeFlawHandlersByKey.  Entity key: " << it->first);
      const FlawHandlerEntry& entry(it->second);
      for(FlawHandlerEntry::const_iterator fIt = entry.begin(); fIt != entry.end(); ++fIt)
        condDebugMsg(!fIt->second.isValid(), "FlawManager:isValid", getId() << " Invalid flaw handler id in m_activeFlawHandlersByKey.  Entity key: " << it->first);
    }
    return true;
  }

    void FlawManager::initialize(const TiXmlElement& configData, 
                                 const PlanDatabaseId db, 
                                 const ContextId ctx, 
                                 const FlawManagerId parent)
    {
      checkError(m_db.isNoId(), "Can only be initialized once.");
      m_db = db;
      m_parent = parent;
      m_context = ctx;
      m_ceListener = boost::make_shared<FlawManager::Listener>(this);
      
      EngineId engine = m_db->getEngine();
      m_flawFilters = (new MatchingEngine(engine,configData,"FlawFilter"))->getId();
      m_flawHandlers = (new MatchingEngine(engine,configData,"FlawHandler"))->getId();
      
      for(std::set<MatchingRuleId>::const_iterator it = m_flawFilters->getRules().begin(); it != m_flawFilters->getRules().end(); ++it) {
        MatchingRuleId rule = *it;
        check_error(rule.isValid());
        debugMsg("FlawManager:initialize", "Setting context on " << rule);
        rule->setContext(m_context);
      }
      for(std::set<MatchingRuleId>::const_iterator it = m_flawHandlers->getRules().begin(); it != m_flawHandlers->getRules().end(); ++it) {
        MatchingRuleId rule = *it;
        check_error(rule.isValid());
        debugMsg("FlawManager:initialize", "Setting context on " << rule);
        rule->setContext(m_context);
      }
      handleInitialize();
      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
    }

  IteratorId FlawManager::createIterator(){ 
    checkRuntimeError(ALWAYS_FAIL, "Should never get here.");
    return IteratorId::noId();
  }

  void FlawManager::notifyAdded(const ConstraintId) {}
  void FlawManager::notifyRemoved(const ConstraintId) {
    // Check if it's the correct type (FlawHandler::VariableListener)
    // then we search through the map and remove any instances of it.
    // if(Id<FlawHandler::VariableListener>::convertable(constraint)) {
    //   debugMsg("FlawManager:notifyRemoved:Constraint", getId() << "->notifyRemoved( " <<
    //            constraint->getName() << "(" << constraint->getKey() << ") )");
    //   Id<FlawHandler::VariableListener> listener = constraint;
        
    //   // As the target of a FlawHandler::VariableListener is only set by its constructor,
    //   // copying such a constraint (as in a merge) will leave its target set to NULL.
    //   if(listener->getTarget().isValid()) {
    //     eint targetKey = listener->getTarget()->getKey();
    //     double weight = listener->getHandler()->getWeight();

    //     debugMsg("FlawManager:notifyRemoved:Constraint", "Looking for active flaw handlers on target key.");
    //     std::map<eint, FlawHandlerEntry>::iterator activeFlawHandlerEntry = m_activeFlawHandlersByKey.find(targetKey);
    //     if(activeFlawHandlerEntry != m_activeFlawHandlersByKey.end()) {
    //       debugMsg("FlawManager:notifyRemoved:Constraint", "Found one.");
    //       FlawHandlerEntry& entry = activeFlawHandlerEntry->second;
    //       FlawHandlerEntry::iterator eit = entry.find(weight);
    //       while(eit != entry.end() && eit->first == weight) {
    //         if(eit->second == listener->getHandler()) {
    //           //debugMsg("FlawManager:erase:handler", getId() << " Removing " << eit->second->toString() << " from flaw handler entry for " << targetKey);
    //           debugMsg("FlawManager:erase:handler", " [" << __FILE__ << ":" << __LINE__ << "] removing flaw handler entry with key " << targetKey);
    //           entry.erase(eit);
    //           break;
    //         }
    //         ++eit;
    //       }
    //     }
    //   }

    //   for(std::multimap<eint, boost::shared_ptr<FlawHandlerWorker> >::iterator it = m_flawHandlerGuards.begin(); it != m_flawHandlerGuards.end();) {
    //     if(it->second.get() == listener) {
    //       debugMsg("FlawManager:notifyRemoved:Constraint", "Removing ("<< constraint->getKey() << ") from m_flawHandlerGuards, its constraint is deleted.");
    //       check_error_variable(unsigned long size = m_flawHandlerGuards.size());
    //       debugMsg("FlawManager:erase:guards", " [" << __FILE__ << ":" << __LINE__ << "] removing entry with key " << it->first << " from m_flawHandlerGuards");
    //       m_flawHandlerGuards.erase(it++);
    //       debugMsg("FlawManager:notifyRemoved:Constraint", "m_flawHandlerGuards.size() == " << m_flawHandlerGuards.size());
    //       checkError(m_flawHandlerGuards.size() == size - 1, "Map size not consistant with erasure: " << size << " - 1 != " << m_flawHandlerGuards.size());
    //     }
    //     else
    //       ++it;
    //   }
    // }
  }

    /**
     * Remove flaw handler guard constraints for this variable, and if it is a state
     * variable also remove for the token.
     */
    void FlawManager::notifyRemoved(const ConstrainedVariableId var){
      debugMsg("FlawManager:notifyRemoved", getId() << " Removing active flaw handlers and guards for " << var->getName() << "(" << var->getKey() << ")");
      
      for(std::multimap<eint, boost::shared_ptr<FlawHandler::VariableListener> >::iterator it = m_flawHandlerGuards.find(var->getKey());
          it != m_flawHandlerGuards.end() && it->first == var->getKey();) {
        debugMsg("FlawManager:notifyRemoved", getId() << " Removing a " << typeid(it->second).name());
	boost::shared_ptr<FlawHandler::VariableListener>& cid = it->second;
        debugMsg("FlawManager:erase:guards", " [" << __FILE__ << ":" << __LINE__ << "] removing entry with key " << var->getKey() << " from m_flawHandlerGuards");
	debugMsg("FlawManager:discard", " [" << __FILE__ << ":" << __LINE__ << "] discarding a constraint in m_flawHandlerGuards: " << cid);

        m_flawHandlerGuards.erase(it++);
        // delete static_cast<Constraint*>(cid);
      }
      condDebugMsg(m_flawHandlerGuards.find(var->getKey()) != m_flawHandlerGuards.end(), "FlawManager:erase:guards", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->getKey() << " from m_flawHandlerGuards");
      m_flawHandlerGuards.erase(var->getKey());

      condDebugMsg(m_activeFlawHandlersByKey.find(var->getKey()) != m_activeFlawHandlersByKey.end(), "FlawManager:erase:active", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->getKey() << " from m_activeFlawHandlersByKey");
      m_activeFlawHandlersByKey.erase(var->getKey());

      condDebugMsg(m_staticFiltersByKey.find(var->getKey()) != m_staticFiltersByKey.end(), "FlawManager:erase:static", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->getKey() << " from m_staticFiltersByKey");
      m_staticFiltersByKey.erase(var->getKey());

      condDebugMsg(m_dynamicFiltersByKey.find(var->getKey()) != m_dynamicFiltersByKey.end(), "FlawManager:erase:dynamic", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->getKey() << " from m_dynamicFiltersByKey");
      m_dynamicFiltersByKey.erase(var->getKey());

      // Handle a guard variable getting removed before the flawed variable does.
      for(std::multimap<eint, boost::shared_ptr<FlawHandler::VariableListener> >::iterator it = m_flawHandlerGuards.begin(); it != m_flawHandlerGuards.end();) {
        debugMsg("FlawManager:notifyRemoved", "Removing from a guard in m_flawHandlerGuards.");
        if(std::find(it->second->scope().begin(),it->second->scope().end(),var) != it->second->scope().end()) {
          debugMsg("FlawManager:nonexecuting", "This doesn't/shouldn't execute (According to MJI)");

	  boost::shared_ptr<FlawHandler::VariableListener>& listener = it->second;
          // As the target of a FlawHandler::VariableListener is only set by its constructor,
          // copying such a constraint (as in a merge) will leave its target set to NULL.
          if(listener->getTarget().isValid()) {
            eint targetKey = listener->getTarget()->getKey();
            double weight = listener->getHandler()->getWeight();

            debugMsg("FlawManager:notifyRemoved", "Looking for active flaw handlers on target key.");
            std::map<eint, FlawHandlerEntry>::iterator activeFlawHandlerEntry = m_activeFlawHandlersByKey.find(targetKey);
            if(activeFlawHandlerEntry != m_activeFlawHandlersByKey.end()) {
              debugMsg("FlawManager:notifyRemoved", "Found one.");
              FlawHandlerEntry& entry = activeFlawHandlerEntry->second;
              for(FlawHandlerEntry::iterator eit = entry.find(weight);eit != entry.end() && eit->first == weight; ++eit) {
                if(eit->second == listener->getHandler()) {
                  debugMsg("FlawManager:notifyRemoved", getId() << " Removing " << eit->second->toString() << " from flaw handler entry for " << targetKey);
                  entry.erase(eit);
                  break;
                }
              }
            }
            m_flawHandlerGuards.erase(it++);
          }
          else
            ++it;
        }
        else
          ++it;
      }

      if(Token::isStateVariable(var)){
        debugMsg("FlawManager:notifyRemoved", 
                 getId() << " Variable " << var->getName() << "(" << var->getKey() << ") is a state variable.  Removing flaw handlers and guards for " <<
                 id_cast<Token>(var->parent())->getPredicateName());
        eint parentKey = var->parent()->getKey();
        debugMsg("FlawManager:notifyRemoved", "Parent Key: " << parentKey);
        debugMsg("FlawManager:notifyRemoved", "m_flawHandlerGuards.size() == " << m_flawHandlerGuards.size());
        for(std::multimap<eint, boost::shared_ptr<FlawHandler::VariableListener> >::iterator it = m_flawHandlerGuards.find(parentKey);
            it != m_flawHandlerGuards.end() && it->first == parentKey;) {
          check_error(it->second);
	  boost::shared_ptr<FlawHandler::VariableListener>& cid = it->second;
          debugMsg("FlawManager:erase:guards", " [" << __FILE__ << ":" << __LINE__ << "] removing entry with key " << var->parent()->getKey() << " from m_flawHandlerGuards");
	  debugMsg("FlawManager:discard", " [" << __FILE__ << ":" << __LINE__ << "] discarding a constraint in m_flawHandlerGuards: " << cid);

          m_flawHandlerGuards.erase(it++);
          // delete static_cast<Constraint*>(cid);
        }
        condDebugMsg(m_flawHandlerGuards.find(var->parent()->getKey()) != m_flawHandlerGuards.end(), "FlawManager:erase:guards", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->parent()->getKey() << " from m_flawHandlerGuards");
        m_flawHandlerGuards.erase(var->parent()->getKey());

        condDebugMsg(m_activeFlawHandlersByKey.find(var->parent()->getKey()) != m_activeFlawHandlersByKey.end(), "FlawManager:erase:active", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->parent()->getKey() << " from m_activeFlawHandlersByKey");
        m_activeFlawHandlersByKey.erase(var->parent()->getKey());

        condDebugMsg(m_staticFiltersByKey.find(var->parent()->getKey()) != m_staticFiltersByKey.end(), "FlawManager:erase:static", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->parent()->getKey() << " from m_staticFiltersByKey");
        m_staticFiltersByKey.erase(var->parent()->getKey());

        condDebugMsg(m_dynamicFiltersByKey.find(var->parent()->getKey()) != m_dynamicFiltersByKey.end(), "FlawManager:erase:dynamic", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << var->parent()->getKey() << " from m_dynamicFiltersByKey");
        m_dynamicFiltersByKey.erase(var->parent()->getKey());
      }
      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
    }

  void FlawManager::notifyChanged(const ConstrainedVariableId ,
                                  const DomainListener::ChangeType& ){
    //TODO: do work to determine which notifications need disseminating here
  }


  void FlawManager::notifyAdded(const TokenId){}
    void FlawManager::notifyRemoved(const TokenId token) {
      debugMsg("FlawManager:notifyRemoved", getId() << " Removing active flaw handlers and guards for " << token->getPredicateName() <<
               "(" << token->getKey() << ")");
      for(std::multimap<eint, boost::shared_ptr<FlawHandler::VariableListener> >::iterator it = m_flawHandlerGuards.find(token->getKey());
          it != m_flawHandlerGuards.end() && it->first == token->getKey();) {
	boost::shared_ptr<FlawHandler::VariableListener>& cid = it->second;
        debugMsg("FlawManager:erase:guards", " [" << __FILE__ << ":" << __LINE__ << "] removing entry with key " << token->getKey() << " from m_flawHandlerGuards");
	debugMsg("FlawManager:discard", " [" << __FILE__ << ":" << __LINE__ << "] discarding a constraint in m_flawHandlerGuards: " << cid);
        m_flawHandlerGuards.erase(it++);

        // delete static_cast<Constraint*>(cid);
      }
      condDebugMsg(m_flawHandlerGuards.find(token->getKey()) != m_flawHandlerGuards.end(), "FlawManager:erase:guards", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << token->getKey() << " from m_flawHandlerGuards");
      m_flawHandlerGuards.erase(token->getKey());

      condDebugMsg(m_activeFlawHandlersByKey.find(token->getKey()) != m_activeFlawHandlersByKey.end(), "FlawManager:erase:active", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << token->getKey() << " from m_activeFlawHandlersByKey");
      m_activeFlawHandlersByKey.erase(token->getKey());

      condDebugMsg(m_staticFiltersByKey.find(token->getKey()) != m_staticFiltersByKey.end(), "FlawManager:erase:static", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << token->getKey() << " from m_staticFiltersByKey");
      m_staticFiltersByKey.erase(token->getKey());

      condDebugMsg(m_dynamicFiltersByKey.find(token->getKey()) != m_dynamicFiltersByKey.end(), "FlawManager:erase:dynamic", " [" << __FILE__ << ":" << __LINE__ << "] removing entries with key " << token->getKey() << " from m_dynamicFiltersByKey");
      m_dynamicFiltersByKey.erase(token->getKey());

      condDebugMsg(!isValid(), "FlawManager:isValid", getId() << " Invalid datastructures in flaw manager.");
    }

    DecisionPointId FlawManager::nextZeroCommitmentDecision(){
      return DecisionPointId::noId();
    }

    DecisionPointId FlawManager::next(Priority& bestPriority){
      EntityId flawToResolve;

      // Cannot do better if we already have a best case priority
      if(bestPriority == getBestCasePriority())
        return DecisionPointId::noId();

      // Initialize the prority to beat
      Priority bestP =  bestPriority - (2 * cast_double(EPSILON));
      IteratorId it = createIterator();

      std::string explanation = "unknown";
      // Now go through the candidates
      while(!it->done()){

        // Get the next flaw candidate
        const EntityId candidate = it->next();
        checkError(candidate.isValid(), "Iterator bug returning a noId");
        checkError(!dynamicMatch(candidate), "Iterator bug allowing " << candidate->toString());

        debugMsg("FlawManager:next", "Evaluating " << candidate->toString() << " to beat " << bestP);

        Priority priority = getPriority(candidate);

        // >= +EPSILON if priority < bestP
        // <= -EPSILON if priority > bestP
        // > -EPSILON && < EPSILON if priority == bestP
        Priority priorityDiff = bestP - priority; 

        debugMsg("FlawManager:next", "Got priority " << priority);
        // If we have a better candidate

        //is this a bug?
        if(priorityDiff >= EPSILON){
          debugMsg("FlawManager:next", "Updating because priority " << priority << 
                   " is better than old best (" << bestP << ")");          
          flawToResolve = candidate;
          bestP = priority;
          explanation = "priority";
          debugMsg("FlawManager:next", "Updating flaw to resolve " << candidate->getKey() << ") " << candidate->toString());          
          if(bestP == getBestCasePriority())
            break;
        }
        else if((std::abs(priorityDiff) < EPSILON && betterThan(candidate, flawToResolve, explanation))){
          debugMsg("FlawManager:next",
                   "Updating because candidate is judged better than old candidate.");
          flawToResolve = candidate;
          bestP = priority;
          //explanation = "preference";
          debugMsg("FlawManager:next", "Updating flaw to resolve (" << candidate->getKey() << ") " << candidate->toString());          
          if(bestP == getBestCasePriority())
            break;
        }
        
        //         condDebugMsg(priorityDiff <= -EPSILON || std::abs(priorityDiff) < EPSILON, "FlawManager:next", "Priority for " << candidate->toString() << " (" << 
        //                      priority << ") is not better than the current best (" << bestP << ")");
        //         condDebugMsg(std::abs(priorityDiff) < EPSILON && !betterThan(candidate, flawToResolve), "FlawManager:next", "Candidate " <<
        //                      candidate->toString() << " isn't better than the current best flaw (" << 
        //                      flawToResolve->toString() << ")");
      }

      delete static_cast<Iterator*>(it);

      DecisionPointId decision;
      if(flawToResolve.isId()){
        bestPriority = bestP;
        decision = allocateDecisionPoint(flawToResolve, explanation);
      }

      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
      return decision;
    }

    bool FlawManager::inScope(const EntityId entity) {
      checkError(m_db->getConstraintEngine()->constraintConsistent(), 
                 "Assumes the database is constraint consistent but it is not.");
      IteratorId it = createIterator();
      bool result = false;
      while(!it->done()){
        if(it->next() == entity){
          result = true;
          break;
        }
      }

      delete static_cast<Iterator*>(it);
      return result;
    }

    /**
     * We always try static matching first since the results are cached and it is generally very fast. Such matching
     * is based on properties of the entity that cannot change. Dynamic matching is based on specific guard values or other
     * dynamic properties that must be re-evaluated on each cycle.
     */
    bool FlawManager::matches(const EntityId entity){
      return staticMatch(entity) || dynamicMatch(entity);
    }

  bool FlawManager::staticMatch(const EntityId entity) {

    // If there is a parent flaw manager ten it may already have excluded the given entity. Filters are inherited in this way.
    if(m_parent.isId() && m_parent->staticMatch(entity)) {
      debugMsg("FlawManager:staticMatch", "Excluding " << entity->getKey() << " based on parent flaw manager.");

      return true;
    }

    condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
    /// If found in the static filters, then return the value
    std::map<eint, bool>::const_iterator it = m_staticFiltersByKey.find(entity->getKey());
    if(it != m_staticFiltersByKey.end()) {
      debugMsg("FlawManager:staticMatch", 
               (it->second ? "Excluding " : "Including ") << entity->getKey() << " because it was previously " <<
               (it->second ? "excluded." : "included."));
      return it->second;
    }

    // Load filters
    std::vector<MatchingRuleId> filters;
    if(TokenId::convertable(entity))
      m_flawFilters->getMatches(TokenId(entity), filters);
    else
      m_flawFilters->getMatches(ConstrainedVariableId(entity), filters);

    std::vector<FlawFilterId> dynamicFilters;
    for(std::vector<MatchingRuleId>::const_iterator fIt = filters.begin();
        fIt != filters.end(); ++fIt){
      FlawFilterId flawFilter = *fIt;
      if(flawFilter->isDynamic())
        dynamicFilters.push_back(flawFilter);
      else { // Static so prune and quit
        debugMsg("FlawManager:staticMatch", getId() << " Sticking " << entity->getKey() << " into static filters.");
        m_staticFiltersByKey.insert(std::make_pair(entity->getKey(), true));
        debugMsg("FlawManager:staticMatch", 
                 flawFilter->getName() <<" excluding " << entity->getKey() << ".  Matched " << flawFilter->toString() << ".");

        return true;
      }
    }

    // If we get here, it is not statically filtered and it we can add the entries, if any, for dynamic filtering
    debugMsg("FlawManager:staticMatch", getId() << " Sticking " << entity->getKey() << " into static filters.");
    m_staticFiltersByKey.insert(std::make_pair(entity->getKey(), false));
    debugMsg("FlawManager:staticMatch", getId() << " Sticking " << entity->getKey() << " into dynamic filters.");
    m_dynamicFiltersByKey.insert(std::make_pair(entity->getKey(), dynamicFilters));
    condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
    return false;
  }

    bool FlawManager::staticallyExcluded(const EntityId entity) const {
      if(m_parent.isId() && m_parent->staticallyExcluded(entity)) {
        debugMsg("FlawManager:staticallyExcluded", "Excluding " << entity->getKey() << " because its parent was statically excluded.");
        return true;
      }

      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
      std::map<eint, bool>::const_iterator it = m_staticFiltersByKey.find(entity->getKey());
      checkError(it != m_staticFiltersByKey.end(), "Filters must not have been queried yet for " << entity->getKey());

      debugMsg("FlawManager:staticallyExcluded", (it->second ? "Excluding " : "Including ") << entity->getKey());
      return it->second;
    }

    bool FlawManager::dynamicMatch(const EntityId entity) {
      checkError(!staticallyExcluded(entity), "Cannot call dynamic match if statically excluded");
      if(m_parent.isId() && m_parent->dynamicMatch(entity)){
	debugMsg("FlawManager:dynamicMatch",  "Excluding " << entity->getKey() << " because of parent.");
        return true;
      }

      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
      Eint2FlawFilterVectorMap::const_iterator it = m_dynamicFiltersByKey.find( entity->getKey() );

      if(it != m_dynamicFiltersByKey.end()){
        const std::vector<FlawFilterId>& dynamicFilters = it->second;
        for(std::vector<FlawFilterId>::const_iterator it_a = dynamicFilters.begin(); it_a != dynamicFilters.end(); ++it_a){
          FlawFilterId dynamicFilter = *it_a;
          checkError(dynamicFilter->isDynamic(), "Must be a bug in construction code.");
	  debugMsg("FlawManager:dynamicMatch",  "Evaluating " << entity->getKey() << ". "  <<                   
		     dynamicFilter->getName() << " excluding " << entity->toString() << " with " << dynamicFilter->toString());

          if(dynamicFilter->test(entity)){
            debugMsg("FlawManager:dynamicMatch",  "Excluding " << entity->getKey() << ". "  <<                   
		     dynamicFilter->getName() << " excluding " << entity->toString() << " with " << dynamicFilter->toString());

            condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
            return true;
          }
        }
      }

      debugMsg("FlawManager:dynamicMatch",  "Including " << entity->getKey());
      return false;
    }

    Priority FlawManager::getPriority(const EntityId entity){
      debugMsg("FlawManager:getPriority", "Getting priority for " << entity->getKey());
      FlawHandlerId flawHandler = getFlawHandler(entity);
      checkError(flawHandler.isValid(), "No flawHandler for " << entity->toString());
      debugMsg("FlawManager:getPriority", "Returning priority " << flawHandler->getPriority(entity));
      return flawHandler->getPriority(entity);
    }

    /**
     * @brief Now we conduct a simple match where we select based on first avalaible.
     */
    DecisionPointId FlawManager::allocateDecisionPoint(const EntityId entity, const std::string& explanation){
      static unsigned int sl_counter(0); // Helpful for debugging
      sl_counter++;

      FlawHandlerId flawHandler = getFlawHandler(entity);
      checkError(flawHandler.isValid(), "On " << sl_counter << ": No flawHandler for " << entity->toString());
      DecisionPointId dp =  flawHandler->create(m_db->getClient(), entity, explanation);
      dp->setCutoff(flawHandler->getMaxChoices());
      return dp;
    }

  FlawHandlerId FlawManager::getFlawHandler(const EntityId entity){
    condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
    static unsigned int sl_counter(0); // Helpful for debugging
    sl_counter++;

    // First, try to find if there is one available already
    checkError(entity.isValid(), entity);
    checkError(m_db->getConstraintEngine()->constraintConsistent(), "Must be propagated and consistent.");

    std::map<eint, FlawHandlerEntry >::const_iterator it = m_activeFlawHandlersByKey.find(entity->getKey());
    if(it != m_activeFlawHandlersByKey.end()){
      FlawHandlerEntry entry = it->second;
      FlawHandlerEntry::const_iterator entryIt = entry.end();
      FlawHandlerId flawHandler = (--entryIt)->second;

      debugMsg("FlawManager:getFlawHandler", "returning handler " << flawHandler->toString() <<
               " with priority " << flawHandler->getPriority() << " for key " << entity->getKey());
      return flawHandler;
    }
    else { // Have to load heuristics
      debugMsg("FlawManager:getFlawHandler", "Loading heuristics.");
      std::vector<MatchingRuleId> candidates;
      m_flawHandlers->getMatches(entity, candidates);

      debugMsg("FlawManager:getFlawHandler", "There are " << candidates.size() << " flaw handlers to consider.");
      FlawHandlerEntry entry;
      bool requiresPropagation = false;
      for(std::vector<MatchingRuleId>::const_iterator cIt = candidates.begin();
          cIt!= candidates.end(); ++cIt){
        FlawHandlerId candidate = *cIt;
        debugMsg("FlawManager:getFlawHandler",
                 "Evaluating flaw handler " << candidate->toString() << " for (" <<
                 entity->getKey() << ")");

        // If it does not pass a custom static match then ignore it
        if(!candidate->customStaticMatch(entity))
          continue;

        if(candidate->hasGuards()){
          std::vector<ConstrainedVariableId> guards;
          // Make the scope. If cant match up, then ignore this handler
          if(!candidate->makeConstraintScope(entity, guards))
            continue;

	  boost::shared_ptr<FlawHandler::VariableListener> guardListener =
	    //boost::make_shared<FlawHandlerWorker>(entity,
	    boost::make_shared<FlawHandler::VariableListener>(m_db->getConstraintEngine(),
							      entity,
							      getId(),
							      candidate,
							      guards);
          requiresPropagation = true;
          debugMsg("FlawManager:getFlawHandler", getId() << " Sticking " << entity->getKey() <<
                   " into flaw handler guards (Guard listener: " << guardListener <<").");
          m_flawHandlerGuards.insert(std::make_pair(entity->getKey(),
						    guardListener));
          // If we are not yet ready to move on.
          if(!candidate->test(guards))
            continue;
        }

        // Now insert in the list according to the weight. This will give the highest weight as the last entry
        entry.insert(std::pair<double, FlawHandlerId>(candidate->getWeight(), candidate));
        debugMsg("FlawManager:getFlawHandler", "Added active FlawHandler " << candidate->toString() << std::endl << " for entity " << entity->getKey());
      }
      debugMsg("FlawManager:getFlawHandler", "Found " << entry.size() << " possible heuristics" << " for entity " << entity->getKey());
      checkError(!entry.empty(), "No Heuristics for " << entity->toString());

      debugMsg("FlawManager:getFlawHandler", getId() << " Sticking " << entity->getKey() << " into active flaw handlers.");
      m_activeFlawHandlersByKey.insert(std::make_pair(entity->getKey(), entry));

      // Propagate if necessary to process any constraints that have been added. This is required
      // so that we get the most up to date flaw handler
      if(requiresPropagation){
        m_db->getConstraintEngine()->propagate();
        synchronize();
      }

      // Now call recursively, but should get a hit this time
      debugMsg("FlawManager:getFlawHandler", "Making recursive call.");
      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
      return getFlawHandler(entity);
    }
  }

    void FlawManager::notifyActivated(const EntityId target, const FlawHandlerId flawHandler){
      check_error(target.isValid());
      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
      std::map<eint, FlawHandlerEntry>::iterator it = m_activeFlawHandlersByKey.find(target->getKey());
      checkError(it != m_activeFlawHandlersByKey.end(), 
                 "We should have at least one entry for a standard handler for entity " << target->getKey() << " handler " << flawHandler->toString());
      FlawHandlerEntry& entry = it->second;
      entry.insert(std::pair<double, FlawHandlerId>(flawHandler->getWeight(),flawHandler ));
      debugMsg("FlawManager:notifyActivated", "Added active FlawHandler " << flawHandler->toString() << std::endl << " for entity " << target->getKey());
      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
    }

    void FlawManager::notifyDeactivated(const EntityId target, const FlawHandlerId flawHandler){
      condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
      std::map<eint, FlawHandlerEntry>::iterator it = m_activeFlawHandlersByKey.find(target->getKey());
      checkError(it != m_activeFlawHandlersByKey.end(), 
                 "We should have at least one entry for a standard handler for " << target->getKey() << " handler " << flawHandler->toString());

      FlawHandlerEntry& entry = it->second;

      for(FlawHandlerEntry::iterator handlerIt = entry.begin(); handlerIt != entry.end(); ++handlerIt){
        if(handlerIt->second == flawHandler){
          entry.erase(handlerIt);
          condDebugMsg(!isValid(), "FlawManager:isValid", "Invalid datastructures in flaw manger.");
          return;
        }
      }
    }

    bool FlawManager::betterThan(const EntityId a, const EntityId b, std::string& explanation){
      if(a.isId() && b.isId()) {
        explanation = "higherKey";
        return (a->getKey() > b->getKey());
      }
      else {
        explanation = "b.isNoId";
        checkError(b.isNoId(), b);
        return true;
      }
    }

    std::string FlawManager::toString(const EntityId entity) const {
      return entity->toString();
    }

    /**
     * @brief test if the timestamp is current
     */
    bool FlawManager::inSynch() const {
      return m_timestamp == m_db->getConstraintEngine()->cycleCount();
    }

    /**
     * @brief Update the timestamp
     */
    void FlawManager::synchronize(){
      m_timestamp =  m_db->getConstraintEngine()->cycleCount();
    }


    /** FLAW ITERATOR IMPLEMENTION **/

FlawIterator::FlawIterator(FlawManager& manager)
    : m_visited(0), 
      m_done(false),
      m_manager(manager),
      m_flaw() {

  // Force the manager to synch up with the cycle count of the constraint engine
  manager.synchronize();
}

    void FlawIterator::advance(){
      EntityId candidate = nextCandidate();
      while(candidate.isId()){
	checkError(candidate.isValid(), candidate);
        if(m_manager.dynamicMatch(candidate))
          candidate = nextCandidate();
        else {
          m_flaw = candidate;
          return;
        }
      }

      m_done = true;
      m_flaw = TokenId::noId();
    }

    bool FlawIterator::done() const {
      return m_done;
    }

    const EntityId FlawIterator::next() {
      check_error(m_manager.inSynch(), "Error: stale flaw iterator.");
      checkError(!done(), "Cannot be done when you call next.");

      EntityId flaw = m_flaw;
      advance();
      ++m_visited;
      return flaw;
    }

void FlawManager::updateGuards(const ConstrainedVariable& var) {
  typedef std::multimap<eint, boost::shared_ptr<FlawHandler::VariableListener> > WorkerMap;
  for(std::pair<WorkerMap::iterator,
          WorkerMap::iterator> range = m_flawHandlerGuards.equal_range(var.getKey());
      range.first != range.second;
      ++range.first) {
    range.first->second->doWork();
  }
}

}
}
