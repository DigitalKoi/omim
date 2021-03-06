#include "testing/testing.hpp"

#include "search/categories_cache.hpp"
#include "search/emitter.hpp"
#include "search/intermediate_result.hpp"
#include "search/model.hpp"
#include "search/pre_ranker.hpp"
#include "search/ranker.hpp"
#include "search/search_integration_tests/helpers.hpp"
#include "search/search_tests_support/test_search_engine.hpp"
#include "search/suggest.hpp"

#include "indexer/categories_holder.hpp"
#include "indexer/feature_algo.hpp"
#include "indexer/features_vector.hpp"
#include "indexer/mwm_set.hpp"
#include "indexer/scales.hpp"

#include "generator/generator_tests_support/test_feature.hpp"
#include "generator/generator_tests_support/test_mwm_builder.hpp"

#include "geometry/mercator.hpp"

#include "platform/country_defines.hpp"
#include "platform/local_country_file.hpp"

#include "base/assert.hpp"
#include "base/cancellable.hpp"
#include "base/limited_priority_queue.hpp"
#include "base/math.hpp"
#include "base/stl_add.hpp"

#include "std/algorithm.hpp"
#include "std/iterator.hpp"
#include "std/vector.hpp"

using namespace generator::tests_support;
using namespace search::tests_support;

namespace search
{
namespace
{
class TestRanker : public Ranker
{
public:
  TestRanker(TestSearchEngine & engine, Emitter & emitter, vector<Suggest> const & suggests,
             VillagesCache & villagesCache, my::Cancellable const & cancellable,
             vector<PreResult1> & results)
    : Ranker(static_cast<Index const &>(engine), engine.GetCountryInfoGetter(), emitter,
             GetDefaultCategories(), suggests, villagesCache, cancellable)
    , m_results(results)
  {
  }

  inline bool Finished() const { return m_finished; }

  // Ranker overrides:
  void SetPreResults1(vector<PreResult1> && preResults1) override
  {
    CHECK(!Finished(), ());
    move(preResults1.begin(), preResults1.end(), back_inserter(m_results));
    preResults1.clear();
  }

  void UpdateResults(bool lastUpdate) override
  {
    CHECK(!Finished(), ());
    if (lastUpdate)
      m_finished = true;
  }

private:
  vector<PreResult1> & m_results;
  bool m_finished = false;
};

class PreRankerTest : public SearchTest
{
public:
  vector<Suggest> m_suggests;
  my::Cancellable m_cancellable;
};

UNIT_CLASS_TEST(PreRankerTest, Smoke)
{
  // Tests that PreRanker correctly computes distances to pivot when
  // number of results is larger than batch size, and that PreRanker
  // emits results nearest to the pivot.

  m2::PointD const kPivot(0, 0);
  m2::RectD const kViewport(m2::PointD(-5, -5), m2::PointD(5, 5));

  size_t const kBatchSize = 50;

  vector<TestPOI> pois;

  for (int x = -5; x <= 5; ++x)
  {
    for (int y = -5; y <= 5; ++y)
    {
      pois.emplace_back(m2::PointD(x, y), "cafe", "en");
      pois.back().SetTypes({{"amenity", "cafe"}});
    }
  }

  TEST_LESS(kBatchSize, pois.size(), ());

  auto mwmId = BuildCountry("Cafeland", [&](TestMwmBuilder & builder) {
    for (auto const & poi : pois)
      builder.Add(poi);
  });

  vector<PreResult1> results;
  Emitter emitter;
  VillagesCache villagesCache(m_cancellable);
  TestRanker ranker(m_engine, emitter, m_suggests, villagesCache, m_cancellable, results);

  PreRanker preRanker(m_engine, ranker, pois.size());
  PreRanker::Params params;
  params.m_viewport = kViewport;
  params.m_accuratePivotCenter = kPivot;
  params.m_scale = scales::GetUpperScale();
  params.m_batchSize = kBatchSize;
  preRanker.Init(params);
  preRanker.SetViewportSearch(true);

  vector<double> distances(pois.size());
  vector<bool> emit(pois.size());

  FeaturesVectorTest fv(mwmId.GetInfo()->GetLocalFile().GetPath(MapOptions::Map));
  fv.GetVector().ForEach([&](FeatureType & ft, uint32_t index) {
    FeatureID id(mwmId, index);
    preRanker.Emplace(id, PreRankingInfo(SearchModel::SEARCH_TYPE_POI, TokenRange(0, 1)));

    TEST_LESS(index, pois.size(), ());
    distances[index] = MercatorBounds::DistanceOnEarth(feature::GetCenter(ft), kPivot);
    emit[index] = true;
  });

  preRanker.UpdateResults(true /* lastUpdate */);

  TEST(all_of(emit.begin(), emit.end(), IdFunctor()), (emit));
  TEST(ranker.Finished(), ());
  TEST_EQUAL(results.size(), kBatchSize, ());

  vector<bool> checked(pois.size());
  for (size_t i = 0; i < results.size(); ++i)
  {
    size_t const index = results[i].GetId().m_index;
    TEST_LESS(index, pois.size(), ());

    TEST(!checked[index], (index));
    TEST(my::AlmostEqualAbs(distances[index], results[i].GetDistance(), 1e-3),
         (distances[index], results[i].GetDistance()));
    checked[index] = true;
  }
}
}  // namespace
}  // namespace search
