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
LPAStar::LPAStar(int start_location, int goal_location, const float* my_heuristic, const MapLoader* ml) :
  my_heuristic(my_heuristic), my_map(ml->my_map), actions_offset(ml->moves_offset) {
  this->start_location = start_location;
  this->goal_location = goal_location;
  this->map_size = map_size;
  this->search_iterations = 0;
  this->num_expanded.push_back(0);
  this->paths.push_back(vector<int>());
  this->paths_costs.push_back(0);
  this->expandedHeatMap.push_back(vector<int>());

  // Initialize allNodes_table (hash table) and OPEN (heap).
  empty_node = new LPANode();
  empty_node->loc_id_ = -1;
  deleted_node = new LPANode();
  deleted_node->loc_id_ = -2;
  allNodes_table.set_empty_key(empty_node);
  allNodes_table.set_deleted_key(deleted_node);
  open_list.clear();
  allNodes_table.clear();

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
  allNodes_table[start_n] = start_n;

  // Create goal node. (Not being pushed to OPEN.)
  goal_n = new LPANode(goal_location,
                       std::numeric_limits<float>::max(),  // g_val
                       std::numeric_limits<float>::max(),  // v_val
                       my_heuristic[goal_location],         // h_val
                       nullptr,                             // bp
                       std::numeric_limits<int>::max());    // t
  allNodes_table[goal_n] = goal_n;
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
void LPAStar::updatePath(LPANode* goal) {
  this->paths.push_back(vector<int>());
  this->paths_costs.push_back(0);

  LPANode* curr = goal;
  while (curr != start_n) {
    VLOG(11) << curr->nodeString();
    paths[search_iterations].push_back(curr->loc_id_);
    curr = curr->bp_;
  }
  paths[search_iterations].push_back(start_location);
  reverse(paths[search_iterations].begin(),
          paths[search_iterations].end());

  paths_costs[search_iterations] = goal->g_;
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
void LPAStar::addVertexConstraint(int loc_id, int ts) {
  VLOG_IF(1, ts == 0) << "We assume vertex constraints cannot happen at timestep 0.";
  // 1) Invalidate this node (that is, sets bp_=nullptr, g=INF, v=INF) and remove from OPEN.
  LPANode* n = retrieveNode(loc_id, ts).second;
  // "Invalidates" n (that is, sets bp_=nullptr, g=INF, v=INF) and remove from OPEN.
  n->initState();
  if (n->in_openlist_ == true) {
    openlistRemove(n);
  }
  for (int direction = 0; direction < 5; direction++) {
    auto succ_loc_id = loc_id + actions_offset[direction];
    if (!my_map[succ_loc_id]) {
      addEdgeConstraint(loc_id, succ_loc_id, ts+1);
      addEdgeConstraint(succ_loc_id, loc_id, ts);
    }
  }
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
void LPAStar::addEdgeConstraint(int from_id, int to_id, int ts) {
  dcm.addEdgeConstraint(from_id, to_id, ts);
  LPANode* n = retrieveNode(to_id, ts).second;
  if (n->bp_ != nullptr && n->bp_->loc_id_ == from_id) {
    updateState(n, false);
  }
}
// ----------------------------------------------------------------------------


/*
 * Retrieves a pointer to a node:
 * 1) if it was already generated before, it is retrieved from the hash table and returned (along with true)
 * 2) if this state is seen first, a new node is generated (and initialized) and then put into the hash table and returned (along with false)
*/
// ----------------------------------------------------------------------------
inline std::pair<bool, LPANode*> LPAStar::retrieveNode(int loc_id, int t) {  // (t=0 for single agent)
  // create dummy node to be used for table lookup
  LPANode* temp_n = new LPANode(loc_id,
                                std::numeric_limits<float>::max(),  // g_val
                                std::numeric_limits<float>::max(),  // v_val
                                my_heuristic[loc_id],                // h_val
                                nullptr,                             // bp
                                t);                                  // timestep
  hashtable_t::iterator it;
  // try to retrieve it from the hash table
  it = allNodes_table.find(temp_n);
  if ( it == allNodes_table.end() ) {  // case (2) above
    //num_generated[search_iterations]++; -- counted instead when adding to OPEN (so we account for reopening).
    //temp_n->initState();  -- already done correctly in construction above.
    allNodes_table[temp_n] = temp_n;
    VLOG(11) << "\t\t\t\t\tallNodes_table: Added new node" << temp_n->nodeString();
    return (make_pair(false, temp_n));
  } else {  // case (1) above
    delete(temp_n);
    VLOG(11) << "\t\t\t\t\tallNodes_table: Returned existing" << (*it).second->nodeString();
    return (make_pair(true, (*it).second));
  }
  return (make_pair(true, nullptr));  // should never get here...
}
// ----------------------------------------------------------------------------


// Adds a node (that was already initalized via retrieveNode) to OPEN
// ----------------------------------------------------------------------------
inline void LPAStar::openlistAdd(LPANode* n) {
  n->openlist_handle_ = open_list.push(n);
  n->in_openlist_ = true;
}
// ----------------------------------------------------------------------------


// Updates the priority
// ----------------------------------------------------------------------------
inline void LPAStar::openlistUpdate(LPANode* n) {
  //open_list.increase(n->openlist_handle_); -- note -- incremental search: costs can increase or decrease.
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
  for (auto n : allNodes_table) {
    delete(n.second);  // n is std::pair<Key, Data*>
  }
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
inline void LPAStar::printAllNodesTable() {
  cout << "Printing all nodes in the hash table:" << endl;
  for (auto n : allNodes_table) {
    cout << "\t" << (n.second)->stateString() << " ; Address:" << (n.second) << endl;  // n is std::pair<Key, Data*>
  }
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
inline LPANode* LPAStar::retrieveMinPred(LPANode* n) {
  VLOG(11) << "\t\t\t\tretreiveMinPred: before " << n->nodeString();
  LPANode* retVal = nullptr;
  auto best_vplusc_val = std::numeric_limits<float>::max();
  for (int direction = 0; direction < 5; direction++) {
    auto pred_loc_id = n->loc_id_ - actions_offset[direction];
    if (!my_map[pred_loc_id] && !dcm.isDynCons(pred_loc_id,n->loc_id_,n->t_)) {
      auto pred_n = retrieveNode(pred_loc_id, n->t_-1).second; // n->t_ - 1 is pred_timestep
      if (pred_n->v_ + 1 <= best_vplusc_val) {  // Assumes unit edge costs.
        best_vplusc_val = pred_n->v_ + 1;  // Assumes unit edge costs.
        retVal = pred_n;
      }
    }
  }
  VLOG_IF(11, retVal == nullptr) << "\t\t\t\tretreiveMinPred: min is ****NULL**** BAD!!";
  VLOG_IF(11, retVal != nullptr) << "\t\t\t\tretreiveMinPred: min is " << retVal->nodeString();
  return retVal;
}
// ----------------------------------------------------------------------------


// TODO: note2 -- pred_is_overconsistent used for optimization (section 6 of LPA*).
// note -- we assume that if s was already never visited (/generated) via a call to retrieveNode earlier.
// ----------------------------------------------------------------------------
inline void LPAStar::updateState(LPANode* n, bool pred_is_overconsistent) {
  if (n != start_n) {
    VLOG(7) << "\t\tupdateState: Start working on " << n->nodeString();
    n->bp_ = retrieveMinPred(n);
    if (n->bp_ == nullptr /*|| dcm.isDynConst(n->loc_id_, -1, n->t_+1)*/ ) {  // This node is a "dead-end" or has vertex constraint on it.
      n->initState();
      if (n->in_openlist_ == true) {
        openlistRemove(n);
      }
      return;
    }
    n->g_ = (n->bp_)->v_ + 1;  // If we got to this point this traversal is legal (Assumes edges have unit cost).
    VLOG(7) << "\t\tupdateState: After updating bp -- " << n->nodeString();
    if ( !n->isConsistent() ) {
      if (n->in_openlist_ == false) {
        openlistAdd(n);
        VLOG(7) << "\t\t\tand *PUSHED* to OPEN";
      } else {  // node is already in OPEN
        openlistUpdate(n);
        VLOG(7) << "\t\t\tand *UPDATED* in OPEN";
      }
    } else {  // n is consistent
      if (n->in_openlist_ == true) {
        openlistRemove(n);
        VLOG(7) << "\t\t\tand *REMOVED* from OPEN";
      }
    }
    // If goal was found with better priority then update the relevant node.
    if (n->loc_id_ == goal_location &&  // TODO: MAPF has additional time restrictions on goal condition...
        nodes_comparator(n, goal_n) == false) {
//      delete(goal_n);
      VLOG(7) << "\t\tupdateState: Goal node update -- from " << goal_n->nodeString() << " to " << n->nodeString();
      goal_n = n;
    }
  }
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
bool LPAStar::findPath() {

  search_iterations++;
  num_expanded.push_back(0);
  expandedHeatMap.push_back(vector<int>());

  VLOG(5) << "*** Starting LPA* findPath() ***";
  while ( nodes_comparator( open_list.top(), goal_n ) == false ||  // open.minkey < key(goal).
          goal_n->v_ < goal_n->g_) {  // Safe if both are numeric_limits<float>::max.
    VLOG(5) << "OPEN: { " << openToString(true) << " }\n";
    auto curr = openlistPopHead();
    VLOG(5) << "\tPopped node: " << curr->nodeString();
    if (curr->v_ > curr->g_) {  // Overconsistent (v>g).
      VLOG(7) << "(it is *over*consistent)";
      curr->v_ = curr->g_;
      for (int direction = 0; direction < 5; direction++) {
        auto next_loc_id = curr->loc_id_ + actions_offset[direction];
        if (!my_map[next_loc_id]/* &&
            !dcm.isDynCons(curr->loc_id_, next_loc_id, curr->t_+1)*/) {
          auto next_n = retrieveNode(next_loc_id, curr->t_+1);
          updateState(next_n.second, true);
        }
      }
    } else {  // Underconsistent (v<g).
      VLOG(7) << "(it is *under*consistent)";
      curr->v_ = std::numeric_limits<float>::max();
      updateState(curr);  // should we remove it if it is an illegal move?
      for (int direction = 0; direction < 5; direction++) {
        auto next_loc_id = curr->loc_id_ + actions_offset[direction];
        if (!my_map[next_loc_id]/* &&
            !dcm.isDynConst(curr->loc_id_, next_loc_id, curr->t_+1)*/) {
          auto next_n = retrieveNode(next_loc_id, curr->t_+1);
          updateState(next_n.second, false);
        }
      }
    }
  }
  if (goal_n->g_ < std::numeric_limits<float>::max()) {  // If a solution found.
    updatePath(goal_n);
    return true;
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
map_size(other.map_size),
actions_offset(other.actions_offset),
dcm(other.dcm)
{
  search_iterations = 0;
  num_expanded.push_back(0);
  paths.push_back(vector<int>());
  paths_costs.push_back(0);
  expandedHeatMap.push_back(vector<int>());
  empty_node = new LPANode(*(other.empty_node));
  deleted_node = new LPANode(*(other.deleted_node));
  // Create a deep copy of each node and store it in the new Hash table.
  allNodes_table.set_empty_key(empty_node);
  allNodes_table.set_deleted_key(deleted_node);
//  int x1 = allNodes_table.size();
//  int y1 = other.allNodes_table.size();
  //allNodes_table = other.allNodes_table;
//  int x2 = allNodes_table.size();
//  int y2 = other.allNodes_table.size();
  // Map
  for (auto n : other.allNodes_table) {
  //for (auto n : allNodes_table) {
//    if (n.first == other.empty_node)
//      continue;
//    if (n.first == other.deleted_node)
//      continue;
//    if (n.first == empty_node)
//      continue;
//    if (n.first == deleted_node)
//      continue;
//    if (n.first == nullptr)
//      continue;
//    if (n.second == nullptr)
//      continue;
//    if ((long int) (n.second) < 0x10000)
//      continue;
    allNodes_table[n.first] = new LPANode(*(n.second));  // n is std::pair<Key, Data*>.
    //allNodes_table[n.second] = new LPANode(*(n.second));  // n is std::pair<Key, Data*>.
  }
//  int x3 = allNodes_table.size();
//  int y3 = other.allNodes_table.size();
  // Reconstruct the OPEN list with the cloned nodes.
  // This is efficient enough since FibHeap has amortized constant time insert.
  for (auto it = other.open_list.ordered_begin(); it != other.open_list.ordered_end(); ++it) {
    LPANode* n = allNodes_table[*it];
    n->openlist_handle_ = open_list.push(n);
  }
//  int z = allNodes_table.size();
  // Update the backpointers of all cloned versions.
  // (before this its bp_ is the original pointer, but we can use the state in it to
  // retrieve the new clone from the newly built hash table).
  for (auto n : allNodes_table) {
//    int t = allNodes_table.size();
//    if (n.second == nullptr)
//      continue;
    if (n.second->bp_ != nullptr) {
//      if (allNodes_table[n.second->bp_] == nullptr)
//        continue;
      n.second->bp_ = allNodes_table[n.second->bp_];
    }
  }
  // Update start and goal nodes.
  start_n = allNodes_table[other.start_n];
  goal_n = allNodes_table[other.goal_n];
  // nodes_comparator = 
}
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
LPAStar::~LPAStar() {
  releaseNodesMemory();
  delete(empty_node);
  delete(deleted_node);
}
// ----------------------------------------------------------------------------