#ifndef FOOLGO_SRC_PLAYER_UCT_PLAYER_H_

#define FOOLGO_SRC_PLAYER_UCT_PLAYER_H_

#include <atomic>
#include <boost/lexical_cast.hpp>
#include <log4cplus/logger.h>
#include <cassert>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <future>
#include <mutex>
#include <memory>
#include <thread>

#include "../board/force.h"
#include "../board/full_board.h"
#include "../board/position.h"
#include "../game/monte_carlo_game.h"
#include "node_record.h"
#include "passable_player.h"
#include "transposition_table.h"

namespace foolgo {
namespace player {

template<board::BoardLen BOARD_LEN>
class UctPlayer : public PassablePlayer<BOARD_LEN> {
 public:
  UctPlayer(uint32_t seed, int mc_game_count_per_move, int thread_count);

 protected:
  board::PositionIndex NextMoveWithPlayableBoard(
      const board::FullBoard<BOARD_LEN> &full_board);

 private:
  int mc_game_count_per_move_;
  TranspositionTable<BOARD_LEN> transposition_table_;
  uint32_t seed_;
  int thread_count_;
  mutable std::mutex wait_search_mutex_;
  mutable std::condition_variable condition_variable_;
  mutable std::mutex mutex_;

  static log4cplus::Logger logger_;

  void SearchAndModifyNodes(const board::FullBoard<BOARD_LEN> &full_board,
                            std::atomic<int> *mc_game_count_ptr,
                            std::atomic<bool> *is_end_ptr);
  board::PositionIndex MaxUcbChild(
      const board::FullBoard<BOARD_LEN> &full_board);
  float ModifyAverageProfitAndReturnNewProfit(
      board::FullBoard<BOARD_LEN> *full_board_ptr,
      std::atomic<int> *mc_game_count_ptr);
  board::PositionIndex BestChild(const board::FullBoard<BOARD_LEN> &full_board);
  void LogProfits(const board::FullBoard<BOARD_LEN> &full_board);
};

template<board::BoardLen BOARD_LEN>
log4cplus::Logger UctPlayer<BOARD_LEN>::logger_ =
    log4cplus::Logger::getInstance("foolgo.player.UctPlayer");

namespace {

float Ucb(const NodeRecord &node_record, int visited_count_sum) {
  assert(node_record.GetVisitedTime() > 0);
  return node_record.GetAverageProfit()
      + sqrt(2 * log(visited_count_sum) / node_record.GetVisitedTime());
}

template<board::BoardLen BOARD_LEN>
float GetRegionRatio(const board::FullBoard<BOARD_LEN> &full_board,
                board::Force force) {
  int black_region = full_board.BlackRegion();
  float black_ratio = static_cast<float>(black_region)
      / board::BoardLenSquare<BOARD_LEN>();
  return force == board::Force::BLACK_FORCE ? black_ratio : 1 - black_ratio;
}

}

template<board::BoardLen BOARD_LEN>
UctPlayer<BOARD_LEN>::UctPlayer(uint32_t seed, int mc_game_count_per_move,
                                int thread_count)
    : seed_(seed),
      mc_game_count_per_move_(mc_game_count_per_move),
      thread_count_(thread_count) {}

template<board::BoardLen BOARD_LEN>
board::PositionIndex UctPlayer<BOARD_LEN>::NextMoveWithPlayableBoard(
      const board::FullBoard<BOARD_LEN> &full_board) {
  std::atomic<int> current_mc_game_count(0);
  std::atomic<bool> is_end;
  std::vector<std::future<void>> futures;

  SearchAndModifyNodes(full_board, &current_mc_game_count, &is_end);

//  for (int i=0; i<thread_count_; ++i) {
//    auto f = std::async(std::launch::async,
//                        &UctPlayer<BOARD_LEN>::SearchAndModifyNodes, this,
//                        std::ref(full_board),
//                        &current_mc_game_count,
//                        &is_end);
//    futures.push_back(std::move(f));
//  }
//
//  if (!is_end.load()) {
//    std::unique_lock<std::mutex> lock(wait_search_mutex_);
//    condition_variable_.wait(lock);
//  }

  LogProfits(full_board);

  return BestChild(full_board);
}

template<board::BoardLen BOARD_LEN>
void UctPlayer<BOARD_LEN>::SearchAndModifyNodes(
    const board::FullBoard<BOARD_LEN> &full_board,
    std::atomic<int> *mc_game_count_ptr,
    std::atomic<bool> *is_end_ptr) {
  while (*mc_game_count_ptr < mc_game_count_per_move_) {
    board::PositionIndex max_ucb_index = MaxUcbChild(full_board);
//    LOG4CPLUS_DEBUG(logger_, "max_ucb_index:" << max_ucb_index);
    board::FullBoard<BOARD_LEN> max_ucb_child;
    max_ucb_child.Copy(full_board);
    board::Play(&max_ucb_child, max_ucb_index);
    ModifyAverageProfitAndReturnNewProfit(&max_ucb_child, mc_game_count_ptr);
  }

  is_end_ptr->store(true);

  condition_variable_.notify_one();
}

template<board::BoardLen BOARD_LEN>
board::PositionIndex UctPlayer<BOARD_LEN>::MaxUcbChild(
    const board::FullBoard<BOARD_LEN> &full_board) {
  std::lock_guard<std::mutex> lock(mutex_);

  board::Force current_force = board::NextForce(full_board);
  auto playable_index_vector = full_board.PlayableIndexes(current_force);

  assert(!playable_index_vector.empty());

  int visited_count_sum = 0;

  for (board::PositionIndex position_index : playable_index_vector) {
    const NodeRecord *node_record_ptr = transposition_table_.GetChild(
        full_board, position_index);
//    LOG4CPLUS_DEBUG(logger_, "position_index:" <<
//                    static_cast<int>(position_index) << " node_record_ptr:" <<
//                    node_record_ptr);
    if (node_record_ptr == nullptr) {
      return position_index;
    }

    visited_count_sum += node_record_ptr->GetVisitedTime();
  }

  float max_ucb = -1.0f;
  board::PositionIndex max_ucb_index = board::POSITION_INDEX_PASS;

  for (board::PositionIndex position_index : playable_index_vector) {
    const NodeRecord *node_record_ptr = transposition_table_.GetChild(
        full_board, position_index);
//    LOG4CPLUS_DEBUG(logger_, "node_record:" << *node_record_ptr);
    if (node_record_ptr->IsInSearch()) {
      continue;
    }
    // It is guaranteed by the above loop that node_record_ptr is not nullptr.
    float ucb = Ucb(*node_record_ptr, visited_count_sum);
//    LOG4CPLUS_DEBUG(logger_, "ucb:" << ucb);
    if (ucb > max_ucb
        && !full_board.IsSuicide(
            board::Move(board::NextForce(full_board), position_index))) {
      max_ucb = ucb;
      max_ucb_index = position_index;
    }
  }

  return max_ucb_index;
}

template<board::BoardLen BOARD_LEN>
float UctPlayer<BOARD_LEN>::ModifyAverageProfitAndReturnNewProfit(
    board::FullBoard<BOARD_LEN> *full_board_ptr,
    std::atomic<int> *mc_game_count_ptr) {
//  LOG4CPLUS_DEBUG(logger_, "full_board:" << full_board_ptr->ToString(false) <<
//                  "mc_game_count:" << *mc_game_count_ptr);

  float new_profit;
  NodeRecord *node_record_ptr = transposition_table_.Get(*full_board_ptr);

  if (node_record_ptr == nullptr) {
    game::MonteCarloGame<BOARD_LEN> monte_carlo_game(*full_board_ptr, seed_);
    if (!full_board_ptr->IsEnd()) {
      monte_carlo_game.Run();
    }
    ++(*mc_game_count_ptr);
    board::Force force = full_board_ptr->LastForce();
    new_profit = GetRegionRatio(monte_carlo_game.GetFullBoard(), force);
    player::NodeRecord node_record(1, new_profit, false);
    transposition_table_.Insert(*full_board_ptr, node_record);
  } else {
    mutex_.lock();
    node_record_ptr->SetIsInSearch(true);
    mutex_.unlock();

    if (full_board_ptr->IsEnd()) {
      ++(*mc_game_count_ptr);
      new_profit = node_record_ptr->GetAverageProfit();
    } else {
      if (full_board_ptr->PlayableIndexes(board::NextForce(*full_board_ptr))
          .empty()) {
        full_board_ptr->Pass(board::NextForce(*full_board_ptr));
      } else {
        board::PositionIndex max_ucb_index = MaxUcbChild(*full_board_ptr);
        board::Play(full_board_ptr, max_ucb_index);
      }
      new_profit = 1.0f
          - ModifyAverageProfitAndReturnNewProfit(full_board_ptr,
                                                  mc_game_count_ptr);
      std::lock_guard<std::mutex> lock(mutex_);
      float previous_profit = node_record_ptr->GetAverageProfit();
      float modified_profit = (previous_profit
          * node_record_ptr->GetVisitedTime() + new_profit)
          / (node_record_ptr->GetVisitedTime() + 1);
      node_record_ptr->SetAverageProfit(modified_profit);
    }

    node_record_ptr->SetVisitedTimes(node_record_ptr->GetVisitedTime() + 1);
  }

  if (node_record_ptr != nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_record_ptr->SetIsInSearch(false);
  }
  return new_profit;
}

template<board::BoardLen BOARD_LEN>
board::PositionIndex UctPlayer<BOARD_LEN>::BestChild(
    const board::FullBoard<BOARD_LEN> &full_board) {
  board::Force force = board::NextForce(full_board);
  auto playable_index_vector = full_board.PlayableIndexes(force);
  int max_visited_count = -1;
  board::PositionIndex most_visited_index;

  for (board::PositionIndex index : playable_index_vector) {
    const NodeRecord *node_record = transposition_table_.GetChild(full_board,
                                                                  index);
    assert(node_record != nullptr);
    if (node_record->GetVisitedTime() > max_visited_count) {
      max_visited_count = node_record->GetVisitedTime();
      most_visited_index = index;
    }
  }

  return most_visited_index;
}

template<board::BoardLen BOARD_LEN>
void UctPlayer<BOARD_LEN>::LogProfits(
    const board::FullBoard<BOARD_LEN> &full_board) {
  auto get_profit_str =
      [this, &full_board](board::PositionIndex position_index) {
    board::Force force = board::NextForce(full_board);
    auto indexes = full_board.PlayableIndexes(force);
    auto it = std::find(indexes.begin(), indexes.end(), position_index);
    if (it == indexes.end()) {
      return 'N' + std::string(3, ' ');
    } else {
      NodeRecord *node_record = transposition_table_.GetChild(full_board,
                                                              position_index);
      assert(node_record != nullptr);
      float profit = node_record->GetAverageProfit();
      return boost::lexical_cast<std::string>(profit).substr(0, 4);
    }
  };

  LOG4CPLUS_DEBUG(logger_,
                 "profits:"<< board::ToString<BOARD_LEN>(get_profit_str, 4));
}

}
}

#endif /* FOOLGO_SRC_PLAYER_UCT_PLAYER_H_ */