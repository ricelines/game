#include "snapshot.hpp"
#include "record/snapshot.hpp"

#include <algorithm>

namespace amber::elaborate {
namespace {
template <class T> bool selected(std::optional<std::span<const T>> selection, const T &value) {
  return !selection || std::find(selection->begin(), selection->end(), value) != selection->end();
}
template <class S> void gap_key(S &s, GapKey &key) {
  snap::field(s, key.term);
  snap::field(s, key.material);
}
template <class S> void answer(S &s, Answer &value) {
  snap::field(s, value.kind);
  snap::field(s, value.rule);
  snap::field(s, value.text);
  snap::field(s, value.note);
}
template <class S> void request(S &s, Request &value) {
  snap::field(s, value.id);
  snap::field(s, value.requester);
  snap::field(s, value.action);
  gap_key(s, value.gap);
  snap::field(s, value.target);
  snap::field(s, value.capture);
  snap::field(s, value.status);
  snap::field(s, value.claimed_by);
  snap::field(s, value.created_step);
  snap::field(s, value.answered_step);
  answer(s, value.answer);
  snap::field(s, value.note);
  snap::field(s, value.display);
}
template <class S> void last_gap(S &s, LastGap &value) {
  snap::field(s, value.action);
  gap_key(s, value.gap);
  snap::field(s, value.target);
  snap::field(s, value.step);
  snap::field(s, value.capture);
  snap::field(s, value.origin);
}
template <class S> void path(S &s, std::filesystem::path &value) {
  std::string text;
  if constexpr (std::is_same_v<S, snap::Output>)
    text = value.string();
  snap::field(s, text);
  if constexpr (std::is_same_v<S, snap::Input>)
    value = std::move(text);
}
template <class S> void capture(S &s, CaptureRef &value) {
  snap::field(s, value.hash);
  path(s, value.path);
  snap::field(s, value.status);
  snap::field(s, value.error);
}
} // namespace
void write_requests(snap::Output &out, const Requests &table, ObjectSelection selection) {
  auto size = static_cast<std::uint32_t>(table.rows.size());
  out.value4b(size);
  std::vector<Request> rows;
  for (const auto &row : table.rows)
    if (row.id && selected(selection, row.target))
      rows.push_back(row);
  out.container(rows, snap::max_collection(), request<snap::Output>);
}
void read_requests(snap::Input &in, Requests &table) {
  std::uint32_t size = 0;
  in.value4b(size);
  table.rows.clear();
  table.rows.resize(size);
  std::vector<Request> rows;
  in.container(rows, snap::max_collection(), request<snap::Input>);
  for (auto &row : rows) {
    if (!row.id || row.id >= size || table.rows[row.id].id)
      throw std::runtime_error("invalid request snapshot identity");
    table.rows[row.id] = std::move(row);
  }
}
void write_gaps(snap::Output &out, const GapCounts &table, GapSelection selection) {
  std::vector<std::pair<GapKey, std::uint64_t>> rows;
  for (const auto &entry : table.rows)
    if (selected(selection, entry.first))
      rows.push_back(entry);
  out.container(rows, snap::max_collection(), [](snap::Output &s, auto &entry) {
    gap_key(s, entry.first);
    snap::field(s, entry.second);
  });
}
void read_gaps(snap::Input &in, GapCounts &table) {
  table.rows.clear();
  std::vector<std::pair<GapKey, std::uint64_t>> rows;
  in.container(rows, snap::max_collection(), [](snap::Input &s, auto &entry) {
    gap_key(s, entry.first);
    snap::field(s, entry.second);
  });
  for (auto &entry : rows)
    table.rows.emplace(entry);
}
void write_last_gaps(snap::Output &out, const LastGaps &table, ObjectSelection selection) {
  std::vector<std::pair<PlayerId, LastGap>> rows;
  for (const auto &entry : table.rows)
    if (selected(selection, entry.second.target))
      rows.push_back(entry);
  out.container(rows, snap::max_collection(), [](snap::Output &s, auto &entry) {
    snap::field(s, entry.first);
    last_gap(s, entry.second);
  });
}
void read_last_gaps(snap::Input &in, LastGaps &table) {
  table.rows.clear();
  std::vector<std::pair<PlayerId, LastGap>> rows;
  in.container(rows, snap::max_collection(), [](snap::Input &s, auto &entry) {
    snap::field(s, entry.first);
    last_gap(s, entry.second);
  });
  for (auto &entry : rows)
    table.rows.emplace(std::move(entry));
}
void write_capture_refs(snap::Output &out, const CaptureRefs &table, HashSelection selection) {
  std::vector<CaptureRef> rows;
  for (const auto &[hash, row] : table.rows)
    if (selected(selection, hash))
      rows.push_back(row);
  out.container(rows, snap::max_collection(), capture<snap::Output>);
}
void read_capture_refs(snap::Input &in, CaptureRefs &table) {
  table.rows.clear();
  std::vector<CaptureRef> rows;
  in.container(rows, snap::max_collection(), capture<snap::Input>);
  for (auto &row : rows) {
    auto hash = row.hash;
    table.rows.emplace(std::move(hash), std::move(row));
  }
}
} // namespace amber::elaborate
