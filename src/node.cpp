#include "lancet/node.h"

#include <algorithm>
#include <array>
#include "lancet/assert_macro.h"
#include <limits>

#include "lancet/utils.h"

namespace lancet {
Node::Node(const Kmer& k) : mer(k), nodeID(k.ID()), quals(k.Length()), covs(k.Length()), labels(k.Length()) {}

auto Node::CanMerge(const Node& buddy, BuddyPosition merge_dir, std::size_t k) const -> bool {
  if (IsMockNode() || buddy.IsMockNode()) return false;
  const auto reverseBuddy = buddy.Orientation() != Orientation();
  return mer.CanMergeKmers(buddy.mer, merge_dir, reverseBuddy, k);
}

void Node::MergeBuddy(const Node& buddy, BuddyPosition dir, std::size_t k) {
  // Everything except edges are merged from buddy into the node
  const auto reverseBuddy = buddy.Orientation() != Orientation();
  Reserve(mer.Length() + buddy.Length() - k + 1);

  mer.MergeBuddy(buddy.mer, dir, reverseBuddy, k);
  quals.MergeBuddy(buddy.quals, dir, reverseBuddy, k);
  covs.MergeBuddy(buddy.covs, dir, reverseBuddy, k);
  labels.MergeBuddy(buddy.labels, dir, reverseBuddy, k);

  if (!bxData.IsEmpty() || !buddy.bxData.IsEmpty()) bxData.Merge(buddy.bxData);

  if (!hpData.IsEmpty() || !buddy.hpData.IsEmpty()) {
    if (hpData.IsEmpty()) hpData = NodeHP(covs);
    const auto buddyHp = buddy.hpData.IsEmpty() ? NodeHP(buddy.covs) : buddy.hpData;
    hpData.MergeBuddy(buddyHp, dir, reverseBuddy, k);
  }
}

void Node::EmplaceEdge(NodeIdentifier dest_id, EdgeKind k) {
  if (dest_id == MOCK_SOURCE_ID || dest_id == MOCK_SINK_ID) numMockEdges++;
  if (dest_id == nodeID) numSelfEdges++;

  const auto result = edgeSet.emplace(dest_id, k);
  if (result.second) {
    orderedEdges.emplace_back(dest_id, k);
    std::sort(orderedEdges.begin(), orderedEdges.end());
  }
}

void Node::EraseEdge(NodeIdentifier dest_id, EdgeKind k) {
  auto itr = edgeSet.find(Edge(dest_id, k));
  if (itr != edgeSet.end()) {
    edgeSet.erase(itr);
    orderedEdges.clear();
    orderedEdges.insert(orderedEdges.end(), edgeSet.cbegin(), edgeSet.cend());
    std::sort(orderedEdges.begin(), orderedEdges.end());
  }
}

static constexpr auto ALL_EDGE_KINDS = std::array<EdgeKind, 4>{EdgeKind::FF, EdgeKind::FR, EdgeKind::RF, EdgeKind::RR};

void Node::EraseEdge(NodeIdentifier dest_id) {
  std::for_each(ALL_EDGE_KINDS.cbegin(), ALL_EDGE_KINDS.cend(), [&](const EdgeKind& ek) { EraseEdge(dest_id, ek); });
}

void Node::ClearEdges() {
  orderedEdges.clear();
  edgeSet.clear();
}

auto Node::HasSelfLoop() const -> bool { return HasConnection(nodeID); }

auto Node::HasConnection(NodeIdentifier dest_id) const -> bool {
  return std::any_of(ALL_EDGE_KINDS.cbegin(), ALL_EDGE_KINDS.cend(),
                     [&](const EdgeKind& ek) { return edgeSet.find(Edge(dest_id, ek)) != edgeSet.end(); });
}

auto Node::NumEdges(Strand direction) const -> std::size_t {
  return std::count_if(edgeSet.cbegin(), edgeSet.cend(), [&direction](const Edge& e) {
    // faux nodes are not supported by real data, they exist only for path travesal, skip them in counts
    return e.SrcDirection() == direction && e.DestinationID() != MOCK_SOURCE_ID && e.DestinationID() != MOCK_SINK_ID;
  });
}

auto Node::NumEdges() const -> std::size_t { return edgeSet.size() - numMockEdges; }

void Node::UpdateQual(std::string_view sv) { quals.Push(sv); }
void Node::UpdateLabel(KmerLabel label) { labels.Push(label); }

void Node::UpdateHPInfo(const ReadInfo& ri, std::uint32_t min_base_qual) {
  const auto bqPass = quals.HighQualPositions(static_cast<double>(min_base_qual));

  if (hpData.IsEmpty()) hpData = NodeHP(covs);
  if (!ri.tenxBarcode.empty() && bxData.IsBXMissing(ri.label, ri.tenxBarcode)) {
    bxData.AddBX(ri.label, ri.strand, ri.tenxBarcode);
    hpData.Update(ri.haplotypeID, ri.label, bqPass);
  }
}

void Node::UpdateCovInfo(const ReadInfo& ri, std::uint32_t min_base_qual, bool is_tenx_mode) {
  const auto bqPass = quals.HighQualPositions(static_cast<double>(min_base_qual));
  if (is_tenx_mode) return covs.Update(BXCount(ri.label, ri.strand), ri.label, ri.strand, bqPass);
  return covs.Update(ri.label, ri.strand, bqPass);
}

void Node::IncrementCov(SampleLabel label, Strand s, std::size_t base_position) {
  covs.Update(label, s, base_position);
  // hp = 0 means Haplotype::Unassigned
  if (HasBXData() && hasHPData()) hpData.Update(0, label, base_position);
}

auto Node::FillColor() const -> std::string { return IsSource() ? "cyan3" : IsSink() ? "yellow2" : labels.FillColor(); }

auto Node::LabelRatio(KmerLabel label) const -> double { return labels.LabelRatio(label); }
auto Node::HasLabel(KmerLabel label) const -> bool { return labels.HasLabel(label); }
auto Node::IsLabelOnly(KmerLabel label) const -> bool { return labels.IsLabelOnly(label); }

auto Node::TotalSampleCount() const -> std::uint16_t {
  return SampleCount(SampleLabel::TUMOR) + SampleCount(SampleLabel::NORMAL);
}
auto Node::SampleCount(SampleLabel label) const -> std::uint16_t { return covs.TotalCov(label); }
auto Node::SampleCount(SampleLabel label, Strand s) const -> std::uint16_t { return covs.StrandCov(label, s); }
auto Node::BXCount(SampleLabel label, Strand s) const -> std::uint16_t { return bxData.BXCount(label, s); }

auto Node::MinSampleBaseCov(bool bq_pass) const -> std::uint16_t {
  auto result = std::numeric_limits<std::uint16_t>::max();
  const auto tmrCovs = covs.BaseCovs(SampleLabel::TUMOR);
  const auto nmlCovs = covs.BaseCovs(SampleLabel::NORMAL);
  LANCET_ASSERT(tmrCovs.size() == nmlCovs.size());  // NOLINT

  for (std::size_t idx = 0; idx < tmrCovs.size(); idx++) {
    const auto totalCov = bq_pass ? tmrCovs[idx].BQPassTotalCov() + nmlCovs[idx].BQPassTotalCov()
                                  : tmrCovs[idx].RawTotalCov() + nmlCovs[idx].RawTotalCov();
    result = std::min<std::uint16_t>(result, totalCov);
  }

  return result;
}

auto Node::LowQualPositions(std::uint32_t min_bq) const -> std::vector<std::size_t> {
  return quals.LowQualPositions(static_cast<double>(min_bq));
}

auto Node::FindMergeableNeighbours() const -> std::vector<NodeNeighbour> {
  if (numSelfEdges != 0 || orderedEdges.size() != 2) return {};

  std::vector<NodeNeighbour> results;
  std::for_each(orderedEdges.cbegin(), orderedEdges.cend(), [&results](const Edge& e) {
    if (e.DestinationID() == MOCK_SOURCE_ID || e.DestinationID() == MOCK_SINK_ID) return;
    results.emplace_back(e);
  });

  return results;
}
}  // namespace lancet
