#include "lpa_star.h"
#include <cstring>
#include <climits>
#include <vector>
#include <list>
#include <utility>
#include <boost/heap/fibonacci_heap.hpp>
#include <sparsehash/dense_hash_map>
#include "lpa_node.h"
#include "g_logging.h"
#include "conflict_avoidance_table.h"

using google::dense_hash_map;
using std::cout;
using std::endl;
using boost::heap::fibonacci_heap;
using std::pair;
using std::tuple;
using std::get;
using std::string;
using std::memcpy;

// ----------------------------------------------------------------------------
LPAStar::LPAStar(int start_location, int goal_location, const float* my_heuristic, const MapLoader* ml, int agent_id) :
    my_heuristic(my_heuristic), my_map(ml->my_map), actions_offset(ml->moves_offset), agent_id(agent_id),
    allNodes_table(ml->map_size()) {
  this->start_location = start_location;
  this->goal_location = goal_location;
  this->min_goal_timestep = 0;
  this->map_rows = ml->rows;
  this->map_cols = ml->cols;
  this->search_iterations = 0;
  this->num_expanded.push_back(0);
  this->paths.push_back(vector<int>());
  this->paths_costs.push_back(0);
  this->expandedHeatMap.push_back(vector<int>());
  open_list.clear();

  dcm.setML(ml);

  // Create start node and push into OPEN (findPath is incremental).
  start_n = new LPANode(start_location,
                        0,
                        std::numeric_limits<float>::max(),
                        my_heuristic[start_location],
                        nullptr,
                        0);
  start_n->openlist_handle_ = open_list.push(start_n);
  start_n->in_openlist_ = true;
  allNodes_table.set(start_location, start_n->t_, start_n);

  // Create goal node. (Not being pushed to OPEN.)
  goal_n = new LPANode(goal_location,
                       std::numeric_limits<float>::max(),  // g_val
                       std::numeric_limits<float>::max(),  // v_val
                       my_heuristic[goal_location],         // h_val
                       nullptr,                             // bp
                       std::numeric_limits<int>::max());    // t
  possible_goals.push_back(goal_n);  // Its t is infinity so it must be in the end
  allNodes_table.set(goal_location, goal_n->t_, goal_n);

  // For the case of the trivial path - the start node is never passed to updateState
  if (start_n->loc_id_ == goal_location &&
      start_n->t_ >= this->min_goal_timestep
  ) {
    VLOG(7) << "\t\tupdateState: Goal node update -- from " << goal_n->nodeString() << " to " << start_n->nodeString();
    goal_n = start_n;
    possible_goals.push_front(goal_n);  // It's the start position - there can't be an earlier goal
  }
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
bool LPAStar::updatePath(LPANode* goal) {
  LPANode* curr = goal;
  while (curr != start_n) {
    if (curr == nullptr)
      return false;
    VLOG(11) << curr->nodeString();
    paths[search_iterations].push_back(curr->loc_id_);
    curr = curr->bp_;
  }
  paths[search_iterations].push_back(start_location);
  reverse(paths[search_iterations].begin(),
          paths[search_iterations].end());

  paths_costs[search_iterations] = goal->g_;
  return true;
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
void LPAStar::addVertexConstraint(int loc_id, int ts, const std::vector < std::unordered_map<int, AvoidanceState > >& cat) {
    VLOG_IF(1, ts == 0) << "We assume vertex constraints cannot happen at timestep 0.";
    // 1) Invalidate this node (that is, sets bp_=nullptr, g=INF, v=INF) and remove from OPEN.
    LPANode* n = retrieveNode(loc_id, ts).second;
    // "Invalidates" n (that is, sets bp_=nullptr, g=INF, v=INF) and remove from OPEN.
    // This is like calling updateState(n) after deleting the edges, but a little faster
    n->initState();
    if (n->in_openlist_ == true) {
        openlistRemove(n);
    }

    // 2) Check if it's a constraint on reaching or staying at the goal, and if so, mark that goal as not found.
    //    Is this necessary or an optimization?
    if (loc_id == goal_n->loc_id_)
        // (There can't be edge conflicts after the goal is reached)
    {
        if (min_goal_timestep < ts + 1)
            min_goal_timestep = ts + 1;
        for (LPANode *possible_goal : possible_goals) {
            if (possible_goal->t_ >= min_goal_timestep) {
                goal_n = possible_goal;
                break;
            }
        }
    }

    // 3) Remove from the dcm edges going into and out of the vertex (it's OK to remove edges that were already removed)
    for (int direction = 0; direction < 5; direction++) {
      auto succ_loc_id = loc_id + actions_offset[direction];
      if (0 <= succ_loc_id && succ_loc_id < map_rows*map_cols && !my_map[succ_loc_id] &&
          abs(succ_loc_id % map_cols - loc_id % map_cols) < 2) {
          dcm.addEdgeConstraint(loc_id, succ_loc_id, ts+1);
          dcm.addEdgeConstraint(succ_loc_id, loc_id, ts);
      }
    }

    // 4) Update all nodes that have it as their bp - only they might have their g affected (rhs in the paper) - this optimization was cancelled because the bp might not be up to date
    for (int direction = 0; direction < 5; direction++) {
        auto succ_loc_id = loc_id + actions_offset[direction];
        if (0 <= succ_loc_id && succ_loc_id < map_rows*map_cols && !my_map[succ_loc_id] &&
            abs(succ_loc_id % map_cols - loc_id % map_cols) < 2
            /*NOT filtering edges blocked by the dcm - those are the ones we want!*/) {
            auto [was_known, to_n] = retrieveNode(succ_loc_id, ts+1);
            {
                updateState(to_n, cat, false);
            }

        }
    }
}

void LPAStar::popVertexConstraint(int loc_id, int ts, const std::vector < std::unordered_map<int, AvoidanceState > >& cat)
{
    VLOG_IF(1, ts == 0) << "We assume vertex constraints cannot happen at timestep 0.";
    for (int direction = 4; direction >= 0; direction--) {
        auto succ_loc_id = loc_id + actions_offset[direction];
        if (0 <= succ_loc_id && succ_loc_id < map_rows*map_cols &&  // valid row
            !my_map[succ_loc_id] &&  // not obstacle
            abs(succ_loc_id % map_cols - loc_id % map_cols) < 2  // valid column
            ) {
            dcm.popEdgeConstraint(succ_loc_id, loc_id, ts);
            dcm.popEdgeConstraint(loc_id, succ_loc_id, ts+1);
        }
    }

    LPANode* n = retrieveNode(loc_id, ts).second;

    if (loc_id == goal_n->loc_id_) {  // Lifting a constraint on a goal location
        if (min_goal_timestep == ts + 1) {
            // Then we're lifting the latest constraint on a goal node and necessarily uncovering an improved goal.

            // Reinserting the node into possible_goals happens in updateState later in the function,
            // along with updating goal_n, because this node is an improved goal

            // Update min_goal_timestep
            this->min_goal_timestep = 0;

            for (int j = ts - 1; j >= start_n->h_; --j) {  // Constraints on entering the goal earlier than it can be reached are meaningless
                bool all_blocked = true;
                for (int direction = 4; direction >= 0; direction--) {
                    auto pred_loc_id = loc_id + actions_offset[direction];
                    if (0 <= pred_loc_id && pred_loc_id < map_rows * map_cols &&  // valid row
                        !my_map[pred_loc_id] &&  // valid edge - not from an obstacle
                        abs(pred_loc_id % map_cols - loc_id % map_cols) < 2 &&  // valid column
                        !dcm.isDynCons(pred_loc_id, loc_id, j)  // the edge isn't constrained
                            ) {
                        all_blocked = false;
                        break;
                    }
                }
                if (all_blocked) {
                    this->min_goal_timestep = j + 1;
                    break;
                }
            }
        }
        else {
            // No need to call updateGoal - this isn't an allowed goal at the moment
        }
    }

    updateState(n, cat, false);

    for (int direction = 4; direction >= 0; direction--) {
        auto succ_loc_id = loc_id + actions_offset[direction];
        if (0 <= succ_loc_id && succ_loc_id < map_rows*map_cols &&  // valid row
            !my_map[succ_loc_id] &&  // not an obstacle
            abs(succ_loc_id % map_cols - loc_id % map_cols) < 2  // valid column
            /*we know the edges aren't blocked by the dcm, we've unblocked them above*/
                ) {
            LPANode* to_n = retrieveNode(succ_loc_id, ts+1).second;
                updateState(to_n, cat, false);

        }
    }

}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
void LPAStar::addEdgeConstraint(int from_id, int to_id, int ts, const std::vector < std::unordered_map<int, AvoidanceState > >& cat) {
  dcm.addEdgeConstraint(from_id, to_id, ts);
  LPANode* to_n = retrieveNode(to_id, ts).second;
    updateState(to_n, cat, false);
}

void LPAStar::popEdgeConstraint(int from_id, int to_id, int ts, const std::vector < std::unordered_map<int, AvoidanceState > >& cat)
{
    dcm.popEdgeConstraint(from_id, to_id, ts);
    LPANode* to_n = retrieveNode(to_id, ts).second;
    // Constraints on staying at the goal are always vertex constraints, so no need to check
    // if min_goal_timestep needs to be updated

        updateState(to_n, cat, false);
}
// ----------------------------------------------------------------------------


/*
 * Retrieves a pointer to a node:
 * 1) if it was already generated before, it is retrieved from the hash table and returned (along with true)
 * 2) if this state is seen for the first time, a new node is generated (and initialized) and then put into the hash table and returned (along with false)
*/
// ----------------------------------------------------------------------------
inline std::pair<bool, LPANode*> LPAStar::retrieveNode(int loc_id, int t) {  // (t=0 for single agent)
  // try to retrieve it from the table
  auto [exists, node] = allNodes_table.get(loc_id, t);
  if (exists) {
      VLOG(11) << "\t\t\t\t\tallNodes_table: Returned existing" << node->nodeString();
      return make_pair(true, node);
  }
  else {  // case (2) above
    auto n = new LPANode(loc_id,
                         std::numeric_limits<float>::max(),  // g_val
                         std::numeric_limits<float>::max(),  // v_val
                         my_heuristic[loc_id],         // h_val
                         nullptr,                     // bp
                         t);                                 // timestep

    //num_generated[search_iterations]++; -- counted instead when adding to OPEN (so we account for reopening).
    //temp_n->initState();  -- already done correctly in construction above.
    allNodes_table.set(loc_id, t, n);
    VLOG(11) << "\t\t\t\t\tallNodes_table: Added new node" << n->nodeString();
    return (make_pair(false, n));
  }
}
// ----------------------------------------------------------------------------


// Adds a node (that was already initialized via retrieveNode) to OPEN
// ----------------------------------------------------------------------------
inline void LPAStar::openlistAdd(LPANode* n) {
  n->openlist_handle_ = open_list.push(n);
  n->in_openlist_ = true;
}
// ----------------------------------------------------------------------------


// Updates the priority
// ----------------------------------------------------------------------------
inline void LPAStar::openlistUpdate(LPANode* n) {
  open_list.update(n->openlist_handle_);
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
inline void LPAStar::openlistRemove(LPANode* n) {
  open_list.erase(n->openlist_handle_);
  n->in_openlist_ = false;
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
inline LPANode* LPAStar::openlistPopHead() {
  LPANode* retVal = open_list.top();
  open_list.pop();
  retVal->in_openlist_ = false;
  num_expanded[search_iterations]++;
  expandedHeatMap[search_iterations].push_back(retVal->loc_id_);
  return retVal;
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
inline void LPAStar::releaseNodesMemory() {
//  for (auto n : allNodes_table) {
//    delete(n.second);  // n is std::pair<Key, Data*>
//  }
//  allNodes_table.clear();
// TODO: Implement support for that in XyzHolder
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
inline void LPAStar::printAllNodesTable() {
//  cout << "Printing all nodes in the hash table:" << endl;
//  for (auto n : allNodes_table) {
//    cout << "\t" << (n.second)->stateString() << " ; Address:" << (n.second) << endl;  // n is std::pair<Key, Data*>
//  }
// TODO: Add support for that in XytHolder
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
inline LPANode* LPAStar::retrieveMinPred(LPANode* n) {
  VLOG(11) << "\t\t\t\tretrieveMinPred: before " << n->nodeString();
  LPANode* retVal = nullptr;
  auto best_vplusc_val = std::numeric_limits<float>::max();
  for (int direction = 0; direction < 5; direction++) {
    auto pred_loc_id = n->loc_id_ - actions_offset[direction];
    if (0 <= pred_loc_id && pred_loc_id < map_rows*map_cols && !my_map[pred_loc_id] &&
        abs(pred_loc_id % map_cols - n->loc_id_ % map_cols) < 2 &&
        !dcm.isDynCons(pred_loc_id,n->loc_id_,n->t_)) {
      auto [existed_before, pred_n] = retrieveNode(pred_loc_id, n->t_-1); // n->t_ - 1 is pred_timestep
      if (
          retVal == nullptr ||
          pred_n->v_ + 1 < best_vplusc_val ||
          (pred_n->v_ + 1 == best_vplusc_val && retVal->conflicts_ > pred_n->conflicts_)
          ) {  // Assumes unit edge costs.
        best_vplusc_val = pred_n->v_ + 1;  // Assumes unit edge costs.
        retVal = pred_n;
      }
    }
  }
  VLOG_IF(11, retVal == nullptr) << "\t\t\t\tretrieveMinPred: min is ****NULL**** BAD!!";
  VLOG_IF(11, retVal != nullptr) << "\t\t\t\tretrieveMinPred: min is " << retVal->nodeString();
  return retVal;
}
// ----------------------------------------------------------------------------


// note -- we assume that n was visited (/generated) via a call to retrieveNode earlier
// note2 -- parameter bp_already_set used for optimization (section 6 of the LPA* paper).
// ----------------------------------------------------------------------------
inline void LPAStar::updateState(LPANode* n, const std::vector < std::unordered_map<int, AvoidanceState > >& cat,
                                 bool bp_already_set) {
  if (n != start_n) {
    VLOG(7) << "\t\tupdateState: Start working on " << n->nodeString();
    if (bp_already_set == false) {
        n->bp_ = retrieveMinPred(n);
        if (n->bp_ == nullptr) {
        }
    }
    if (n->bp_ != nullptr)
    {
        n->g_ = (n->bp_)->v_ + 1;  // If we got to this point this traversal is legal (Assumes edges have unit cost).
                                   // The addition is safe from overflow because v_, g_ are floats.
        int n_itself_conflicts = numOfConflictsForStep(n->bp_->loc_id_, n->loc_id_, n->t_, cat, actions_offset);
        n->conflicts_ = (n->bp_)->conflicts_ + n_itself_conflicts;
    }
    else {
        n->g_ = std::numeric_limits<float>::max();
        n->conflicts_ = 0;  // may be overwritten later when the bp is set
    }
    VLOG(7) << "\t\tupdateVertex: After updating bp -- " << n->nodeString();
    // UpdateVertex from the paper:
    if ( !n->isConsistent() ) {
      if (n->in_openlist_ == false) {
        openlistAdd(n);  // The open list contains all inconsistent nodes
        VLOG(7) << "\t\t\tand *PUSHED* to OPEN";
      } else {  // node is already in OPEN
        openlistUpdate(n);
        VLOG(7) << "\t\t\tand *UPDATED* in OPEN";
      }
    } else {  // n is consistent
      if (n->in_openlist_) {
        openlistRemove(n);
        VLOG(7) << "\t\t\tand *REMOVED* from OPEN";
      }
    }

    if (n->loc_id_ == goal_location &&
        n->t_ >= this->min_goal_timestep &&  // This is allowed to be a goal
        nodes_comparator(n, goal_n) == false &&  // Better than the current goal
        n != goal_n  // Is this check needed?
        ) {  // Not described in the paper, but necessary:
             // If goal was found with better priority, then update the relevant node.
      VLOG(7) << "\t\tupdateState: Goal node update -- from " << goal_n->nodeString() << " to " << n->nodeString();
        for (auto it = possible_goals.begin(); it != possible_goals.end() ; ++it)  {
            if ((*it)->t_ == n->t_) {
                break;  // This node is already in the list, a constraint on it was probably lifted
            }
            else if ((*it)->t_ > n->t_) {
                possible_goals.insert(it, n);  // inserts before the iterator
                break;
            }
        }
        updateGoal(); // Can't just set goal_n to n. If this was a constraint on staying at the goal,
                      // there's a consistent untouched goal earlier
    }
  }
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
bool LPAStar::findPath(const std::vector < std::unordered_map<int, AvoidanceState > >& cat, int fLowerBound, int lastGoalConstraintTimestep) {

  search_iterations++;
  num_expanded.push_back(0);
  expandedHeatMap.push_back(vector<int>());
  if (lastGoalConstraintTimestep + 1 < min_goal_timestep)
      min_goal_timestep = lastGoalConstraintTimestep + 1;
  // Can't use fLowerBound or BPMX to improve h values of new nodes - constraints might be removed later, making the
  // improved h incorrect.

  VLOG(5) << "*** Starting LPA* findPath() ***";
  updateGoal();
  while (open_list.empty() == false &&
         (nodes_comparator( open_list.top(), goal_n ) == false ||  // open.minkey < key(goal).
         goal_n->v_ < goal_n->g_)) {  // Safe when both are numeric_limits<float>::max.
    VLOG(5) << "OPEN: { " << openToString(true) << " }\n";
    auto curr = openlistPopHead();
    VLOG(5) << "\tPopped node: " << curr->nodeString();
    if (curr->v_ > curr->g_) {  // Overconsistent (v>g).
      VLOG(7) << "(it is *over*consistent)";
      curr->v_ = curr->g_;
      for (int direction = 0; direction < 5; direction++) {
        auto next_loc_id = curr->loc_id_ + actions_offset[direction];
        if (0 <= next_loc_id && next_loc_id < map_rows*map_cols && !my_map[next_loc_id] &&
            abs(next_loc_id % map_cols - curr->loc_id_ % map_cols) < 2 &&
            !dcm.isDynCons(curr->loc_id_, next_loc_id , curr->t_+1)) {
            auto next_n = retrieveNode(next_loc_id, curr->t_+1);
            if (next_n.second->g_ > curr->v_ + 1) {
                next_n.second->bp_ = curr;
                //next_n.second->g_ = curr->v_ + 1;  // Done in updateState
                updateState(next_n.second, cat, true);
            }
        }
      }
    } else {  // Underconsistent (v<g).
      VLOG(7) << "(it is *under*consistent)";
      curr->v_ = std::numeric_limits<float>::max();
      updateState(curr, cat);
      for (int direction = 0; direction < 5; direction++) {
        auto next_loc_id = curr->loc_id_ + actions_offset[direction];
        if (0 <= next_loc_id && next_loc_id < map_rows*map_cols && !my_map[next_loc_id] &&
            abs(next_loc_id % map_cols - curr->loc_id_ % map_cols) < 2 &&
            !dcm.isDynCons(curr->loc_id_, next_loc_id , curr->t_+1)) {
          auto next_n = retrieveNode(next_loc_id, curr->t_+1);
          updateState(next_n.second, cat, false);
        }
      }
    }
    updateGoal();
  }
  paths.push_back(vector<int>());
  paths_costs.push_back(0);
  if (goal_n->g_ < std::numeric_limits<float>::max()) {  // If a solution was found.
    return updatePath(goal_n);
  }
  return false;  // No solution found.
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
string LPAStar::openToString(bool print_priorities) const {
  string retVal;
  for (auto it = open_list.ordered_begin(); it != open_list.ordered_end(); ++it) {
    if (print_priorities == true)
      retVal = retVal + (*it)->nodeString() + " ; ";
    else
      retVal = retVal + (*it)->stateString() + " ; ";
  }
  return retVal;
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
// Note -- Fibonacci has amortized constant time insert. That's why rebuilding the heap is linear time.
//         TODO: maybe find out how to create a *deep* copy of the heap (but this isn't easy...)
LPAStar::LPAStar (const LPAStar& other) :
start_location(other.start_location),
goal_location(other.goal_location),
my_heuristic(other.my_heuristic),
my_map(other.my_map),
map_rows(other.map_rows),
map_cols(other.map_cols),
actions_offset(other.actions_offset),
dcm(other.dcm),
min_goal_timestep(other.min_goal_timestep),
agent_id(other.agent_id),
allNodes_table(other.allNodes_table.xy_size)
{
    search_iterations = 0;
    num_expanded.push_back(0);
    paths.push_back(vector<int>());
    paths_costs.push_back(0);
    expandedHeatMap.push_back(vector<int>());
    // Create a deep copy of each node and store it in the new Hash table.
    // Map
    for (int i = 0; i < allNodes_table.xy_size ; ++i) {
        if (other.allNodes_table.data[i] == nullptr)
            continue;
        for (auto [t, n]: *(other.allNodes_table.data[i])) {
            auto copy = new LPANode(*n);
            allNodes_table.set(i, t, copy);
        }
    }
    // Reconstruct the OPEN list with the cloned nodes.
    // This is efficient enough since FibHeap has amortized constant time insert.
    for (auto it = other.open_list.ordered_begin(); it != other.open_list.ordered_end(); ++it) {
        auto [found, n] = allNodes_table.get((*it)->loc_id_, (*it)->t_);
        n->openlist_handle_ = open_list.push(n);
    }
    // Update the backpointers of all cloned versions.
    // (before this its bp_ is the original pointer, but we can use the state in it to
    // retrieve the new clone from the newly built hash table).
    for (int i = 0; i < allNodes_table.xy_size ; ++i) {
        if (allNodes_table.data[i] == nullptr)
            continue;
        for (auto [t, n]: *(allNodes_table.data[i])) {
            if (n->bp_ != nullptr) {
                auto [found, bp] = allNodes_table.get(n->bp_->loc_id_, n->bp_->t_);
                n->bp_ = bp;
            }
        }
    }
    // Update start and goal nodes.
    auto [found_start, start_n] = allNodes_table.get(other.start_n->loc_id_, other.start_n->t_);
    start_n = start_n;
    auto [found_goal, goal_n] = allNodes_table.get(other.goal_n->loc_id_, other.goal_n->t_);
    goal_n = goal_n;

    for (auto possible_goal : other.possible_goals)
    {
        auto [found_possible_goal, possible_goal_n] = allNodes_table.get(possible_goal->loc_id_, possible_goal->t_);
        possible_goals.push_back(possible_goal_n);
    }
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
LPAStar::~LPAStar() {
  releaseNodesMemory();
}

void LPAStar::updateGoal() {
    if (open_list.empty())
        return;
    auto open_list_top = open_list.top();
    auto open_list_top_key = open_list_top->getKey1();
    for (LPANode *possible_goal : possible_goals) {
        if (possible_goal->t_ >= min_goal_timestep &&
            (((nodes_comparator(possible_goal, open_list_top) == false) && (possible_goal->v_ >= possible_goal->g_)) ||  // Goal is consistent or oveconsistent
              possible_goal->t_ >= open_list_top_key)  // Goal is (still) reachable (assuming all steps cost 1),
                                                       // even if we don't know how to reach it yet (v and g could be infinity)
        ) {
            goal_n = possible_goal;
            break;
        }
    }

}
// ----------------------------------------------------------------------------
