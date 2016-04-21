
// Copyright (c) 2015 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.


#include "openMVG/sfm/pipelines/sequential/sequential_SfM.hpp"
#include "openMVG/sfm/pipelines/sfm_robust_model_estimation.hpp"
#include "openMVG/sfm/sfm_data_io.hpp"
#include "openMVG/sfm/sfm_data_BA_ceres.hpp"
#include "openMVG/sfm/sfm_data_filters.hpp"
#include "openMVG/sfm/pipelines/localization/SfM_Localizer.hpp"

#include "openMVG/matching/indMatch.hpp"
#include "openMVG/multiview/essential.hpp"
#include "openMVG/multiview/triangulation.hpp"
#include "openMVG/multiview/triangulation_nview.hpp"
#include "openMVG/graph/connectedComponent.hpp"
#include "openMVG/stl/stl.hpp"
#include "openMVG/system/timer.hpp"

#include "third_party/htmlDoc/htmlDoc.hpp"
#include "third_party/progress/progress.hpp"

#ifdef _MSC_VER
#pragma warning( once : 4267 ) //warning C4267: 'argument' : conversion from 'size_t' to 'const int', possible loss of data
#endif

namespace openMVG {
namespace sfm {

using namespace openMVG::geometry;
using namespace openMVG::cameras;

SequentialSfMReconstructionEngine::SequentialSfMReconstructionEngine(
  const SfM_Data & sfm_data,
  const std::string & soutDirectory,
  const std::string & sloggingFile)
  : ReconstructionEngine(sfm_data, soutDirectory),
    _sLoggingFile(sloggingFile),
    _initialpair(Pair(0,0)),
    _camType(EINTRINSIC(PINHOLE_CAMERA_RADIAL3))
{
  if (!_sLoggingFile.empty())
  {
    // setup HTML logger
    _htmlDocStream = std::make_shared<htmlDocument::htmlDocumentStream>("SequentialReconstructionEngine SFM report.");
    _htmlDocStream->pushInfo(
      htmlDocument::htmlMarkup("h1", std::string("SequentialSfMReconstructionEngine")));
    _htmlDocStream->pushInfo("<hr>");

    _htmlDocStream->pushInfo( "Dataset info:");
    _htmlDocStream->pushInfo( "Views count: " +
      htmlDocument::toString( sfm_data.GetViews().size()) + "<br>");
  }

  // Init remaining image list
  for (Views::const_iterator itV = sfm_data.GetViews().begin();
    itV != sfm_data.GetViews().end(); ++itV)
  {
    _set_remainingViewId.insert(itV->second.get()->id_view);
  }
}

SequentialSfMReconstructionEngine::~SequentialSfMReconstructionEngine()
{
  if (!_sLoggingFile.empty())
  {
    // Save the reconstruction Log
    std::ofstream htmlFileStream(_sLoggingFile.c_str());
    htmlFileStream << _htmlDocStream->getDoc();
  }
}

void SequentialSfMReconstructionEngine::SetFeaturesProvider(Features_Provider * provider)
{
  _features_provider = provider;
}

void SequentialSfMReconstructionEngine::SetMatchesProvider(Matches_Provider * provider)
{
  _matches_provider = provider;
}

// Compute robust Resection of remaining images
// - group of images will be selected and resection + scene completion will be tried
void SequentialSfMReconstructionEngine::RobustResectionOfImages(
  const std::set<size_t>& viewIds,
  std::set<size_t>& set_reconstructedViewId,
  std::set<size_t>& set_rejectedViewId)
{
  size_t imageIndex = 0;
  size_t resectionGroupIndex = 0;
  std::set<size_t> set_remainingViewId(viewIds);
  std::vector<size_t> vec_possible_resection_indexes;
  while (FindImagesWithPossibleResection(vec_possible_resection_indexes, set_remainingViewId))
  {
    auto chrono_start = std::chrono::steady_clock::now();
    std::cout << "Resection group start " << resectionGroupIndex << " with " << vec_possible_resection_indexes.size() << " images.\n";
    bool bImageAdded = false;
    // Add images to the 3D reconstruction
    for (const size_t possible_resection_index: vec_possible_resection_indexes )
    {
      const size_t currentIndex = imageIndex;
      ++imageIndex;
      const bool bResect = Resection(possible_resection_index);
      bImageAdded |= bResect;
      if (!bResect)
      {
        set_rejectedViewId.insert(possible_resection_index);
        std::cout << "\nResection of image " << currentIndex << " ID=" << possible_resection_index << " was not possible." << std::endl;
      }
      else
      {
        set_reconstructedViewId.insert(possible_resection_index);
        std::cout << "\nResection of image: " << currentIndex << " ID=" << possible_resection_index << " succeed." << std::endl;
      }
      set_remainingViewId.erase(possible_resection_index);
    }
    std::cout << "Resection of " << vec_possible_resection_indexes.size() << " new images took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - chrono_start).count() << " msec." << std::endl;

    if (bImageAdded)
    {
      if((resectionGroupIndex % 30) == 0)
      {
        chrono_start = std::chrono::steady_clock::now();
        // Scene logging as ply for visual debug
        std::ostringstream os;
        os << std::setw(8) << std::setfill('0') << resectionGroupIndex << "_Resection";
        Save(_sfm_data, stlplus::create_filespec(_sOutDirectory, os.str(), _sfmdataInterFileExtension), _sfmdataInterFilter);
        std::cout << "Save of file " << os.str() << " took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - chrono_start).count() << " msec." << std::endl;
      }
      std::cout << "Global Bundle start, resection group index: " << resectionGroupIndex << ".\n";
      chrono_start = std::chrono::steady_clock::now();
      int bundleAdjustmentIteration = 0;
      const std::size_t nbOutliersThreshold = 50;
      // Perform BA until all point are under the given precision
      do
      {
        auto chrono2_start = std::chrono::steady_clock::now();
        BundleAdjustment();
        std::cout << "Resection group index: " << resectionGroupIndex << ", bundle iteration: " << bundleAdjustmentIteration
                  << " took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - chrono2_start).count() << " msec." << std::endl;
        ++bundleAdjustmentIteration;
      }
      while (badTrackRejector(4.0, nbOutliersThreshold) != 0);
      std::cout << "Bundle with " << bundleAdjustmentIteration << " iterations took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - chrono_start).count() << " msec." << std::endl;
      chrono_start = std::chrono::steady_clock::now();
      eraseUnstablePosesAndObservations(this->_sfm_data, _minPointsPerPose, _minTrackLength);
      std::cout << "eraseUnstablePosesAndObservations took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - chrono_start).count() << " msec." << std::endl;
    }
    ++resectionGroupIndex;
  }
  // Ensure there is no remaining outliers
  badTrackRejector(4.0, 0);
  eraseUnstablePosesAndObservations(this->_sfm_data, _minPointsPerPose, _minTrackLength);
}

bool SequentialSfMReconstructionEngine::Process()
{
  //-------------------
  //-- Incremental reconstruction
  //-------------------
  if (!InitLandmarkTracks())
    return false;

  // Initial pair choice
  if(_initialpair == Pair(0,0))
  {
    if(!AutomaticInitialPairChoice(_initialpair))
    {
      if(_userInteraction)
      {
        // Cannot find a valid initial pair, try to set it by hand?
        if(!ChooseInitialPair(_initialpair))
          return false;
      }
      else
      {
        return false;
      }
    }
  }
  // Else a starting pair was already initialized before

  // Initial pair Essential Matrix and [R|t] estimation.
  if (!MakeInitialPair3D(_initialpair))
    return false;

  std::set<size_t> reconstructedViewIds;
  std::set<size_t> rejectedViewIds;
  std::size_t nbRejectedLoops = 0;
  do
  {
    reconstructedViewIds.clear();
    rejectedViewIds.clear();

    // Compute robust Resection of remaining images
    // - group of images will be selected and resection + scene completion will be tried
    RobustResectionOfImages(
      _set_remainingViewId,
      reconstructedViewIds,
      rejectedViewIds);
    // Remove all reconstructed views from the remaining views
    for(const size_t v: reconstructedViewIds)
    {
      _set_remainingViewId.erase(v);
    }

    std::cout << "SequenctiamSfM -- nbRejectedLoops: " << nbRejectedLoops << std::endl;
    std::cout << "SequenctiamSfM -- reconstructedViewIds: " << reconstructedViewIds.size() << std::endl;
    std::cout << "SequenctiamSfM -- rejectedViewIds: " << rejectedViewIds.size() << std::endl;
    std::cout << "SequenctiamSfM -- _set_remainingViewId: " << _set_remainingViewId.size() << std::endl;

    ++nbRejectedLoops;
    // Retry to perform the resectioning of all the rejected views,
    // as long as new views are successfully added.
  } while( !reconstructedViewIds.empty() && !_set_remainingViewId.empty() );

  //-- Reconstruction done.
  //-- Display some statistics
  std::cout << "\n\n-------------------------------" << "\n"
    << "-- Structure from Motion (statistics):\n"
    << "-- #Camera calibrated: " << _sfm_data.GetPoses().size()
    << " from " << _sfm_data.GetViews().size() << " input images.\n"
    << "-- #Tracks, #3D points: " << _sfm_data.GetLandmarks().size() << "\n"
    << "-------------------------------" << "\n";

  Histogram<double> h;
  ComputeResidualsHistogram(&h);
  std::cout << "\nHistogram of residuals:" << h.ToString() << std::endl;
    
  Histogram<double> hTracks;
  ComputeTracksLengthsHistogram(&hTracks);
  std::cout << "\nHistogram of tracks length:" << hTracks.ToString() << std::endl;
  
  if (!_sLoggingFile.empty())
  {
    using namespace htmlDocument;
    std::ostringstream os;
    os << "Structure from Motion process finished.";
    _htmlDocStream->pushInfo("<hr>");
    _htmlDocStream->pushInfo(htmlMarkup("h1",os.str()));

    os.str("");
    os << "-------------------------------" << "<br>"
      << "-- Structure from Motion (statistics):<br>"
      << "-- #Camera calibrated: " << _sfm_data.GetPoses().size()
      << " from " <<_sfm_data.GetViews().size() << " input images.<br>"
      << "-- #Tracks, #3D points: " << _sfm_data.GetLandmarks().size() << "<br>"
      << "-------------------------------" << "<br>";
    _htmlDocStream->pushInfo(os.str());

    _htmlDocStream->pushInfo(htmlMarkup("h2","Histogram of reprojection-residuals"));

    
    const std::vector<double> xBin = h.GetXbinsValue();
    htmlDocument::JSXGraphWrapper jsxGraph;
    _htmlDocStream->pushXYChart(xBin, h.GetHist(),"3DtoImageResiduals");
    
    const std::vector<double> xBinTracks = hTracks.GetXbinsValue();
    htmlDocument::JSXGraphWrapper jsxGraphTracks;
    _htmlDocStream->pushXYChart(xBinTracks, hTracks.GetHist(),"3DtoTracksSize");
  }

  return true;
}

/// Select a candidate initial pair
bool SequentialSfMReconstructionEngine::ChooseInitialPair(Pair & initialPairIndex) const
{
  if (_initialpair != Pair(0,0))
  {
    // Internal initial pair is already initialized (so return it)
    initialPairIndex = _initialpair;
  }
  else
  {
    // List Views that supports valid intrinsic
    std::set<IndexT> valid_views;
    for (Views::const_iterator it = _sfm_data.GetViews().begin();
      it != _sfm_data.GetViews().end(); ++it)
    {
      const View * v = it->second.get();
      if( _sfm_data.GetIntrinsics().find(v->id_intrinsic) != _sfm_data.GetIntrinsics().end())
        valid_views.insert(v->id_view);
    }

    if (_sfm_data.GetIntrinsics().empty() || valid_views.empty())
    {
      std::cerr
        << "There is no defined intrinsic data in order to compute an essential matrix for the initial pair."
        << std::endl;
      return false;
    }

    std::cout << std::endl
      << "----------------------------------------------------\n"
      << "SequentialSfMReconstructionEngine::ChooseInitialPair\n"
      << "----------------------------------------------------\n"
      << " Pairs that have valid intrinsic and high support of points are displayed:\n"
      << " Choose one pair manually by typing the two integer indexes\n"
      << "----------------------------------------------------\n"
      << std::endl;

    // Try to list the 10 top pairs that have:
    //  - valid intrinsics,
    //  - valid estimated Fundamental matrix.
    std::vector< size_t > vec_NbMatchesPerPair;
    std::vector<openMVG::matching::PairWiseMatches::const_iterator> vec_MatchesIterator;
    const openMVG::matching::PairWiseMatches & map_Matches = _matches_provider->_pairWise_matches;
    for (openMVG::matching::PairWiseMatches::const_iterator
      iter = map_Matches.begin();
      iter != map_Matches.end(); ++iter)
    {
      const Pair current_pair = iter->first;
      if (valid_views.count(current_pair.first) &&
        valid_views.count(current_pair.second) )
      {
        vec_NbMatchesPerPair.push_back(iter->second.size());
        vec_MatchesIterator.push_back(iter);
      }
    }
    // sort the Pairs in descending order according their correspondences count
    using namespace stl::indexed_sort;
    std::vector< sort_index_packet_descend< size_t, size_t> > packet_vec(vec_NbMatchesPerPair.size());
    sort_index_helper(packet_vec, &vec_NbMatchesPerPair[0], std::min((size_t)10, vec_NbMatchesPerPair.size()));

    for (size_t i = 0; i < std::min((size_t)10, vec_NbMatchesPerPair.size()); ++i) {
      const size_t index = packet_vec[i].index;
      openMVG::matching::PairWiseMatches::const_iterator iter = vec_MatchesIterator[index];
      std::cout << "(" << iter->first.first << "," << iter->first.second <<")\t\t"
        << iter->second.size() << " matches" << std::endl;
    }

    // Ask the user to choose an initial pair (by set some view ids)
    std::cout << std::endl << " type INITIAL pair ids: X enter Y enter\n";
    int val, val2;
    if ( std::cin >> val && std::cin >> val2) {
      initialPairIndex.first = val;
      initialPairIndex.second = val2;
    }
  }

  std::cout << "\nPutative starting pair is: (" << initialPairIndex.first
      << "," << initialPairIndex.second << ")" << std::endl;

  // Check validity of the initial pair indices:
  if (_features_provider->feats_per_view.find(initialPairIndex.first) == _features_provider->feats_per_view.end() ||
      _features_provider->feats_per_view.find(initialPairIndex.second) == _features_provider->feats_per_view.end())
  {
    std::cerr << "At least one of the initial pair indices is invalid."
      << std::endl;
    return false;
  }
  return true;
}

bool SequentialSfMReconstructionEngine::InitLandmarkTracks()
{
  // Compute tracks from matches
  tracks::TracksBuilder tracksBuilder;

  {
    // List of features matches for each couple of images
    const openMVG::matching::PairWiseMatches & map_Matches = _matches_provider->_pairWise_matches;
    std::cout << "\n" << "Track building" << std::endl;

    tracksBuilder.Build(map_Matches);
    std::cout << "\n" << "Track filtering" << std::endl;
    tracksBuilder.Filter(_minInputTrackLength);

    std::cout << "\n" << "Track export to internal struct" << std::endl;
    //-- Build tracks with STL compliant type :
    tracksBuilder.ExportToSTL(_map_tracks);
    tracks::TracksUtilsMap::computeTracksPerView(_map_tracks, _map_tracksPerView);
    
    std::cout << "\n" << "Track stats" << std::endl;
    {
      std::ostringstream osTrack;
      //-- Display stats :
      //    - number of images
      //    - number of tracks
      std::set<size_t> set_imagesId;
      tracks::TracksUtilsMap::ImageIdInTracks(_map_tracksPerView, set_imagesId);
      osTrack << "------------------" << "\n"
        << "-- Tracks Stats --" << "\n"
        << " Number of tracks: " << tracksBuilder.NbTracks() << "\n"
        << " Number of images in tracks: " << set_imagesId.size() << "\n";
//        << " Images Id: " << "\n";
//      std::copy(set_imagesId.begin(),
//        set_imagesId.end(),
//        std::ostream_iterator<size_t>(osTrack, ", "));
      osTrack << "\n------------------" << "\n";

      std::map<size_t, size_t> map_Occurence_TrackLength;
      tracks::TracksUtilsMap::TracksLength(_map_tracks, map_Occurence_TrackLength);
      osTrack << "TrackLength, Occurrence" << "\n";
      for (std::map<size_t, size_t>::const_iterator iter = map_Occurence_TrackLength.begin();
        iter != map_Occurence_TrackLength.end(); ++iter)  {
        osTrack << "\t" << iter->first << "\t" << iter->second << "\n";
      }
      osTrack << "\n";
      std::cout << osTrack.str();
    }
  }
  return _map_tracks.size() > 0;
}

bool SequentialSfMReconstructionEngine::AutomaticInitialPairChoice(Pair & initial_pair) const
{
  // From the k view pairs with the highest number of verified matches
  // select a pair that have the largest baseline (mean angle between its bearing vectors).

  const unsigned iMin_inliers_count = 100;
  const float fRequired_min_angle = 3.0f;
  const float fLimit_max_angle = 60.0f; // More than 60 degree, we cannot rely on matches for initial pair seeding

  // List Views that support valid intrinsic (view that could be used for Essential matrix computation)
  std::set<IndexT> valid_views;
  for (Views::const_iterator it = _sfm_data.GetViews().begin();
    it != _sfm_data.GetViews().end(); ++it)
  {
    const View * v = it->second.get();
    if (_sfm_data.GetIntrinsics().count(v->id_intrinsic))
      valid_views.insert(v->id_view);
  }

  if (valid_views.size() < 2)
  {
    std::cerr << "Failed to find an initial pair automatically. There is no view with valid intrinsics." << std::endl;
    return false;
  }

  std::vector<std::pair<double, Pair> > scoring_per_pair;

  // Compute the relative pose & the 'baseline score'
  C_Progress_display my_progress_bar( _matches_provider->_pairWise_matches.size(),
    std::cout,
    "Automatic selection of an initial pair:\n" );
#ifdef OPENMVG_USE_OPENMP
  #pragma omp parallel for schedule(dynamic)
#endif
  for (int i = 0; i < _matches_provider->_pairWise_matches.size(); ++i)
  {
    matching::PairWiseMatches::const_iterator iter = _matches_provider->_pairWise_matches.begin();
    std::advance(iter, i);
    const std::pair< Pair, IndMatches > & match_pair = *iter;

#ifdef OPENMVG_USE_OPENMP
    #pragma omp critical
#endif
    ++my_progress_bar;

    const Pair current_pair = match_pair.first;

    const size_t I = min(current_pair.first, current_pair.second);
    const size_t J = max(current_pair.first, current_pair.second);
    if (!valid_views.count(I) || !valid_views.count(J))
      continue;

    const View * view_I = _sfm_data.GetViews().at(I).get();
    const Intrinsics::const_iterator iterIntrinsic_I = _sfm_data.GetIntrinsics().find(view_I->id_intrinsic);
    const View * view_J = _sfm_data.GetViews().at(J).get();
    const Intrinsics::const_iterator iterIntrinsic_J = _sfm_data.GetIntrinsics().find(view_J->id_intrinsic);

    const Pinhole_Intrinsic * cam_I = dynamic_cast<const Pinhole_Intrinsic*>(iterIntrinsic_I->second.get());
    const Pinhole_Intrinsic * cam_J = dynamic_cast<const Pinhole_Intrinsic*>(iterIntrinsic_J->second.get());
    if (cam_I == NULL || cam_J == NULL)
      continue;

    openMVG::tracks::STLMAPTracks map_tracksCommon;
    const std::set<size_t> set_imageIndex= {I, J};
    tracks::TracksUtilsMap::GetTracksInImagesFast(set_imageIndex, _map_tracks, _map_tracksPerView, map_tracksCommon);

    // Copy points correspondences to arrays for relative pose estimation
    const size_t n = map_tracksCommon.size();
    Mat xI(2,n), xJ(2,n);
    size_t cptIndex = 0;
    for (openMVG::tracks::STLMAPTracks::const_iterator
      iterT = map_tracksCommon.begin(); iterT != map_tracksCommon.end();
      ++iterT, ++cptIndex)
    {
      tracks::submapTrack::const_iterator iter = iterT->second.begin();
      const size_t i = iter->second;
      const size_t j = (++iter)->second;

      Vec2 feat = _features_provider->feats_per_view[I][i].coords().cast<double>();
      xI.col(cptIndex) = cam_I->get_ud_pixel(feat);
      feat = _features_provider->feats_per_view[J][j].coords().cast<double>();
      xJ.col(cptIndex) = cam_J->get_ud_pixel(feat);
    }

    // Robust estimation of the relative pose
    RelativePose_Info relativePose_info;
    relativePose_info.initial_residual_tolerance = Square(4.0);

    bool relativePoseSuccess = robustRelativePose(
      cam_I->K(), cam_J->K(),
      xI, xJ, relativePose_info,
      std::make_pair(cam_I->w(), cam_I->h()), std::make_pair(cam_J->w(), cam_J->h()),
      256);

    if (relativePoseSuccess && relativePose_info.vec_inliers.size() > iMin_inliers_count)
    {
      // Triangulate inliers & compute angle between bearing vectors
      std::vector<float> vec_angles;
      vec_angles.reserve(relativePose_info.vec_inliers.size());
      const Pose3 pose_I = Pose3(Mat3::Identity(), Vec3::Zero());
      const Pose3 pose_J = relativePose_info.relativePose;
      const Mat34 PI = cam_I->get_projective_equivalent(pose_I);
      const Mat34 PJ = cam_J->get_projective_equivalent(pose_J);
      for (const size_t inlier_idx : relativePose_info.vec_inliers)
      {
        Vec3 X;
        TriangulateDLT(PI, xI.col(inlier_idx), PJ, xJ.col(inlier_idx), &X);

        openMVG::tracks::STLMAPTracks::const_iterator iterT = map_tracksCommon.begin();
        std::advance(iterT, inlier_idx);
        tracks::submapTrack::const_iterator iter = iterT->second.begin();
        const Vec2 featI = _features_provider->feats_per_view[I][iter->second].coords().cast<double>();
        const Vec2 featJ = _features_provider->feats_per_view[J][(++iter)->second].coords().cast<double>();
        vec_angles.push_back(AngleBetweenRay(pose_I, cam_I, pose_J, cam_J, featI, featJ));
      }
      // Compute the median triangulation angle
      const unsigned median_index = vec_angles.size() / 2;
      std::nth_element(
        vec_angles.begin(),
        vec_angles.begin() + median_index,
        vec_angles.end());
      const float scoring_angle = vec_angles[median_index];
      // Store the pair iff the pair is in the asked angle range [fRequired_min_angle;fLimit_max_angle]
      if (scoring_angle > fRequired_min_angle &&
          scoring_angle < fLimit_max_angle)
      {
#ifdef OPENMVG_USE_OPENMP
        #pragma omp critical
#endif
        scoring_per_pair.emplace_back(scoring_angle, current_pair);
      }
    }
  }
  std::sort(scoring_per_pair.begin(), scoring_per_pair.end());
  // Since scoring is ordered in increasing order, reverse the order
  std::reverse(scoring_per_pair.begin(), scoring_per_pair.end());
  if (!scoring_per_pair.empty())
  {
    initial_pair = scoring_per_pair.begin()->second;
    return true;
  }
  std::cout << "No valid initial pair found automatically." << std::endl;
  return false;
}

/// Compute the initial 3D seed (First camera t=0; R=Id, second estimated by 5 point algorithm)
bool SequentialSfMReconstructionEngine::MakeInitialPair3D(const Pair & current_pair)
{
  // Compute robust Essential matrix for ImageId [I,J]
  // use min max to have I < J
  const size_t I = min(current_pair.first, current_pair.second);
  const size_t J = max(current_pair.first, current_pair.second);

  // a. Assert we have valid pinhole cameras
  const View * view_I = _sfm_data.GetViews().at(I).get();
  const Intrinsics::const_iterator iterIntrinsic_I = _sfm_data.GetIntrinsics().find(view_I->id_intrinsic);
  const View * view_J = _sfm_data.GetViews().at(J).get();
  const Intrinsics::const_iterator iterIntrinsic_J = _sfm_data.GetIntrinsics().find(view_J->id_intrinsic);

  std::cout << "Initial pair is:\n"
          << "  A - Id: " << I << " - " << " filepath: " << view_I->s_Img_path << "\n"
          << "  B - Id: " << J << " - " << " filepath: " << view_J->s_Img_path << std::endl;

  if (iterIntrinsic_I == _sfm_data.GetIntrinsics().end() ||
      iterIntrinsic_J == _sfm_data.GetIntrinsics().end() )
  {
    std::cerr << "Can't find initial image pair intrinsics: " << view_I->id_intrinsic << ", "  << view_J->id_intrinsic << std::endl;
    return false;
  }

  const Pinhole_Intrinsic * cam_I = dynamic_cast<const Pinhole_Intrinsic*>(iterIntrinsic_I->second.get());
  const Pinhole_Intrinsic * cam_J = dynamic_cast<const Pinhole_Intrinsic*>(iterIntrinsic_J->second.get());
  if (cam_I == NULL || cam_J == NULL)
  {
    std::cerr << "Can't find initial image pair intrinsics (NULL ptr): " << view_I->id_intrinsic << ", "  << view_J->id_intrinsic << std::endl;
    return false;
  }

  // b. Get common features between the two view
  // use the track to have a more dense match correspondence set
  openMVG::tracks::STLMAPTracks map_tracksCommon;
  const std::set<size_t> set_imageIndex= {I, J};
  tracks::TracksUtilsMap::GetTracksInImagesFast(set_imageIndex, _map_tracks, _map_tracksPerView, map_tracksCommon);

  //-- Copy point to arrays
  const size_t n = map_tracksCommon.size();
  Mat xI(2,n), xJ(2,n);
  size_t cptIndex = 0;
  for (openMVG::tracks::STLMAPTracks::const_iterator
    iterT = map_tracksCommon.begin(); iterT != map_tracksCommon.end();
    ++iterT, ++cptIndex)
  {
    tracks::submapTrack::const_iterator iter = iterT->second.begin();
    const size_t i = iter->second;
    const size_t j = (++iter)->second;

    Vec2 feat = _features_provider->feats_per_view[I][i].coords().cast<double>();
    xI.col(cptIndex) = cam_I->get_ud_pixel(feat);
    feat = _features_provider->feats_per_view[J][j].coords().cast<double>();
    xJ.col(cptIndex) = cam_J->get_ud_pixel(feat);
  }
  std::cout << n << " matches in the image pair for the initial pose estimation." << std::endl;

  // c. Robust estimation of the relative pose
  RelativePose_Info relativePose_info;

  const std::pair<size_t, size_t> imageSize_I(cam_I->w(), cam_I->h());
  const std::pair<size_t, size_t> imageSize_J(cam_J->w(), cam_J->h());

  if (!robustRelativePose(
    cam_I->K(), cam_J->K(), xI, xJ, relativePose_info, imageSize_I, imageSize_J, 4096))
  {
    std::cerr << " /!\\ Robust estimation failed to compute E for this pair"
      << std::endl;
    return false;
  }
  std::cout << "A-Contrario initial pair residual: "
    << relativePose_info.found_residual_precision << std::endl;
  // Bound min precision at 1 pix.
  relativePose_info.found_residual_precision = std::max(relativePose_info.found_residual_precision, 1.0);

  bool bRefine_using_BA = true;
  if (bRefine_using_BA)
  {
    // Refine the defined scene
    SfM_Data tiny_scene;
    tiny_scene.views.insert(*_sfm_data.GetViews().find(view_I->id_view));
    tiny_scene.views.insert(*_sfm_data.GetViews().find(view_J->id_view));
    tiny_scene.intrinsics.insert(*_sfm_data.GetIntrinsics().find(view_I->id_intrinsic));
    tiny_scene.intrinsics.insert(*_sfm_data.GetIntrinsics().find(view_J->id_intrinsic));

    // Init poses
    const Pose3 & Pose_I = tiny_scene.poses[view_I->id_pose] = Pose3(Mat3::Identity(), Vec3::Zero());
    const Pose3 & Pose_J = tiny_scene.poses[view_J->id_pose] = relativePose_info.relativePose;

    // Init structure
    const Mat34 P1 = cam_I->get_projective_equivalent(Pose_I);
    const Mat34 P2 = cam_J->get_projective_equivalent(Pose_J);
    Landmarks & landmarks = tiny_scene.structure;

    for (openMVG::tracks::STLMAPTracks::const_iterator
      iterT = map_tracksCommon.begin();
      iterT != map_tracksCommon.end();
      ++iterT)
    {
      // Get corresponding points
      tracks::submapTrack::const_iterator iter = iterT->second.begin();
      const size_t i = iter->second;
      const size_t j = (++iter)->second;

      const Vec2 x1_ = _features_provider->feats_per_view[I][i].coords().cast<double>();
      const Vec2 x2_ = _features_provider->feats_per_view[J][j].coords().cast<double>();

      Vec3 X;
      TriangulateDLT(P1, x1_, P2, x2_, &X);
      Observations obs;
      obs[view_I->id_view] = Observation(x1_, i);
      obs[view_J->id_view] = Observation(x2_, j);
      landmarks[iterT->first].obs = std::move(obs);
      landmarks[iterT->first].X = X;
    }

    Save(tiny_scene, stlplus::create_filespec(_sOutDirectory, "initialPair", _sfmdataInterFileExtension), _sfmdataInterFilter);

    // - refine only Structure and Rotations & translations (keep intrinsic constant)
    Bundle_Adjustment_Ceres::BA_options options(true, false);
    options._linear_solver_type = ceres::DENSE_SCHUR;
    Bundle_Adjustment_Ceres bundle_adjustment_obj(options);
    if (!bundle_adjustment_obj.Adjust(tiny_scene, true, true, false))
    {
      return false;
    }

    // Save computed data
    const Pose3 pose_I = _sfm_data.poses[view_I->id_pose] = tiny_scene.poses[view_I->id_pose];
    const Pose3 pose_J = _sfm_data.poses[view_J->id_pose] = tiny_scene.poses[view_J->id_pose];
    _map_ACThreshold.insert(std::make_pair(I, relativePose_info.found_residual_precision));
    _map_ACThreshold.insert(std::make_pair(J, relativePose_info.found_residual_precision));
    _set_remainingViewId.erase(view_I->id_view);
    _set_remainingViewId.erase(view_J->id_view);

    // List inliers and save them
    for (Landmarks::const_iterator iter = tiny_scene.GetLandmarks().begin();
      iter != tiny_scene.GetLandmarks().end(); ++iter)
    {
      const IndexT trackId = iter->first;
      const Landmark & landmark = iter->second;
      const Observations & obs = landmark.obs;
      Observations::const_iterator iterObs_xI = obs.begin();
      Observations::const_iterator iterObs_xJ = obs.begin();
      std::advance(iterObs_xJ, 1);

      const Observation & ob_xI = iterObs_xI->second;
      const IndexT & viewId_xI = iterObs_xI->first;

      const Observation & ob_xJ = iterObs_xJ->second;
      const IndexT & viewId_xJ = iterObs_xJ->first;

      const double angle = AngleBetweenRay(
        pose_I, cam_I, pose_J, cam_J, ob_xI.x, ob_xJ.x);
      const Vec2 residual_I = cam_I->residual(pose_I, landmark.X, ob_xI.x);
      const Vec2 residual_J = cam_J->residual(pose_J, landmark.X, ob_xJ.x);
      if ( angle > 2.0 &&
           pose_I.depth(landmark.X) > 0 &&
           pose_J.depth(landmark.X) > 0 &&
           residual_I.norm() < relativePose_info.found_residual_precision &&
           residual_J.norm() < relativePose_info.found_residual_precision)
      {
        _sfm_data.structure[trackId] = landmarks[trackId];
      }
    }
    // Save outlier residual information
    Histogram<double> histoResiduals;
    std::cout << std::endl
      << "=========================\n"
      << " MSE Residual InitialPair Inlier: " << ComputeResidualsHistogram(&histoResiduals) << "\n"
      << "=========================" << std::endl;

    if (!_sLoggingFile.empty())
    {
      using namespace htmlDocument;
      _htmlDocStream->pushInfo(htmlMarkup("h1","Essential Matrix."));
      ostringstream os;
      os << std::endl
        << "-------------------------------" << "<br>"
        << "-- Robust Essential matrix: <"  << I << "," <<J << "> images: "
        << view_I->s_Img_path << ","
        << view_J->s_Img_path << "<br>"
        << "-- Threshold: " << relativePose_info.found_residual_precision << "<br>"
        << "-- Resection status: " << "OK" << "<br>"
        << "-- Nb points used for robust Essential matrix estimation: "
        << xI.cols() << "<br>"
        << "-- Nb points validated by robust estimation: "
        << _sfm_data.structure.size() << "<br>"
        << "-- % points validated: "
        << _sfm_data.structure.size()/static_cast<float>(xI.cols())
        << "<br>"
        << "-------------------------------" << "<br>";
      _htmlDocStream->pushInfo(os.str());

      _htmlDocStream->pushInfo(htmlMarkup("h2",
        "Residual of the robust estimation (Initial triangulation). Thresholded at: "
        + toString(relativePose_info.found_residual_precision)));

      _htmlDocStream->pushInfo(htmlMarkup("h2","Histogram of residuals"));

      std::vector<double> xBin = histoResiduals.GetXbinsValue();
      std::pair< std::pair<double,double>, std::pair<double,double> > range =
        autoJSXGraphViewport<double>(xBin, histoResiduals.GetHist());

      htmlDocument::JSXGraphWrapper jsxGraph;
      jsxGraph.init("InitialPairTriangulationKeptInfo",600,300);
      jsxGraph.addXYChart(xBin, histoResiduals.GetHist(), "line,point");
      jsxGraph.addLine(relativePose_info.found_residual_precision, 0,
        relativePose_info.found_residual_precision, histoResiduals.GetHist().front());
      jsxGraph.UnsuspendUpdate();
      jsxGraph.setViewport(range);
      jsxGraph.close();
      _htmlDocStream->pushInfo(jsxGraph.toStr());

      _htmlDocStream->pushInfo("<hr>");

      ofstream htmlFileStream( string(stlplus::folder_append_separator(_sOutDirectory) +
        "Reconstruction_Report.html").c_str());
      htmlFileStream << _htmlDocStream->getDoc();
    }
  }
  return !_sfm_data.structure.empty();
}

double SequentialSfMReconstructionEngine::ComputeResidualsHistogram(Histogram<double> * histo) const
{
  // Collect residuals for each observation
  std::vector<float> vec_residuals;
  vec_residuals.reserve(_sfm_data.structure.size());
  for(Landmarks::const_iterator iterTracks = _sfm_data.GetLandmarks().begin();
      iterTracks != _sfm_data.GetLandmarks().end(); ++iterTracks)
  {
    const Observations & obs = iterTracks->second.obs;
    for(Observations::const_iterator itObs = obs.begin();
      itObs != obs.end(); ++itObs)
    {
      const View * view = _sfm_data.GetViews().find(itObs->first)->second.get();
      const Pose3 pose = _sfm_data.GetPoseOrDie(view);
      const std::shared_ptr<IntrinsicBase> intrinsic = _sfm_data.GetIntrinsics().find(view->id_intrinsic)->second;
      const Vec2 residual = intrinsic->residual(pose, iterTracks->second.X, itObs->second.x);
      vec_residuals.push_back( fabs(residual(0)) );
      vec_residuals.push_back( fabs(residual(1)) );
    }
  }
  // Display statistics
  if (vec_residuals.size() > 1)
  {
    float dMin, dMax, dMean, dMedian;
    minMaxMeanMedian<float>(vec_residuals.begin(), vec_residuals.end(),
                            dMin, dMax, dMean, dMedian);
    if (histo)  {
      *histo = Histogram<double>(dMin, dMax, 10);
      histo->Add(vec_residuals.begin(), vec_residuals.end());
    }

    std::cout << std::endl << std::endl;
    std::cout << std::endl
      << "SequentialSfMReconstructionEngine::ComputeResidualsMSE." << "\n"
      << "\t-- #Tracks:\t" << _sfm_data.GetLandmarks().size() << std::endl
      << "\t-- Residual min:\t" << dMin << std::endl
      << "\t-- Residual median:\t" << dMedian << std::endl
      << "\t-- Residual max:\t "  << dMax << std::endl
      << "\t-- Residual mean:\t " << dMean << std::endl;

    return dMean;
  }
  return -1.0;
}

double SequentialSfMReconstructionEngine::ComputeTracksLengthsHistogram(Histogram<double> * histo) const
{
  // Collect tracks size: number of 2D observations per 3D points
  std::vector<float> vec_nbTracks;
  vec_nbTracks.reserve(_sfm_data.GetLandmarks().size());

  for(Landmarks::const_iterator iterTracks = _sfm_data.GetLandmarks().begin();
      iterTracks != _sfm_data.GetLandmarks().end(); ++iterTracks)
  {
    const Observations & obs = iterTracks->second.obs;
    vec_nbTracks.push_back(obs.size());
  }

  float dMin, dMax, dMean, dMedian;
  minMaxMeanMedian<float>(
    vec_nbTracks.begin(), vec_nbTracks.end(),
    dMin, dMax, dMean, dMedian);

  if (histo)
  {
    *histo = Histogram<double>(dMin, dMax, dMax-dMin+1);
    histo->Add(vec_nbTracks.begin(), vec_nbTracks.end());
  }

  std::cout << std::endl
    << "SequentialSfMReconstructionEngine::ComputeTracksLengthsHistogram." << "\n"
    << "\t-- #Tracks:\t" << _sfm_data.GetLandmarks().size() << std::endl
    << "\t-- Tracks Length min:\t" << dMin << std::endl
    << "\t-- Tracks Length median:\t" << dMedian << std::endl
    << "\t-- Tracks Length max:\t "  << dMax << std::endl
    << "\t-- Tracks Length mean:\t " << dMean << std::endl;

  return dMean;
}

/// Functor to sort a vector of pair given the pair's second value
template<class T1, class T2, class Pred = std::less<T2> >
struct sort_pair_second {
  bool operator()(const std::pair<T1,T2>&left,
                    const std::pair<T1,T2>&right)
  {
    Pred p;
    return p(left.second, right.second);
  }
};

/**
 * @brief Estimate images on which we can compute the resectioning safely.
 *
 * @param[out] vec_possible_indexes: list of indexes we can use for resectioning.
 * @return False if there is no possible resection.
 *
 * Sort the images by the number of features id shared with the reconstruction.
 * Select the image I that share the most of correspondences.
 * Then keep all the images that have at least:
 *  0.75 * #correspondences(I) common correspondences to the reconstruction.
 */
bool SequentialSfMReconstructionEngine::FindImagesWithPossibleResection(
  std::vector<size_t> & vec_possible_indexes,
  std::set<size_t>& set_remainingViewId) const
{
  auto chrono_start = std::chrono::steady_clock::now();
  // Threshold used to select the best images
  static const float dThresholdGroup = 0.75f;

  vec_possible_indexes.clear();

  if (set_remainingViewId.empty() || _sfm_data.GetLandmarks().empty())
    return false;

  // Collect tracksIds
  std::set<size_t> reconstructed_trackId;
  std::transform(_sfm_data.GetLandmarks().begin(), _sfm_data.GetLandmarks().end(),
    std::inserter(reconstructed_trackId, reconstructed_trackId.begin()),
    stl::RetrieveKey());

  Pair_Vec vec_putative; // ImageId, NbPutativeCommonPoint
  #ifdef OPENMVG_USE_OPENMP
    #pragma omp parallel for
  #endif
  for (int i = 0; i < set_remainingViewId.size(); ++i)
  {
    std::set<size_t>::const_iterator iter = set_remainingViewId.cbegin();
    std::advance(iter, i);
    const size_t viewId = *iter;

    // Compute 2D - 3D possible content
    openMVG::tracks::TracksPerView::const_iterator tracksIdsIt = _map_tracksPerView.find(viewId);
    if(tracksIdsIt == _map_tracksPerView.end())
      continue;

    const openMVG::tracks::TracksSet& set_tracksIds = tracksIdsIt->second;
    if (set_tracksIds.empty())
      continue;

    // Count the common possible putative point
    //  with the already 3D reconstructed trackId
    std::vector<size_t> vec_trackIdForResection;
    std::set_intersection(set_tracksIds.begin(), set_tracksIds.end(),
      reconstructed_trackId.begin(),
      reconstructed_trackId.end(),
      std::back_inserter(vec_trackIdForResection));

#ifdef OPENMVG_USE_OPENMP
      #pragma omp critical
#endif
    {
      vec_putative.emplace_back(viewId, vec_trackIdForResection.size());
    }
  }

  // Sort by the number of matches to the 3D scene.
  std::sort(vec_putative.begin(), vec_putative.end(), sort_pair_second<size_t, size_t, std::greater<size_t> >());

  std::size_t minPointsThreshold = 30;

  // If the list is empty or if the list contains images with no correspondences
  // -> (no resection will be possible)
  if (vec_putative.empty() || vec_putative[0].second < minPointsThreshold)
  {
    std::cout << "FindImagesWithPossibleResection failed: ";
    if(vec_putative.empty())
    {
      std::cout << "No putative image.";
    }
    else
    {
      std::cout << "Not enough point in the putative images: ";
      for(auto v: vec_putative)
        std::cout << v.second << ", ";
    }
    std::cout << std::endl;
    // All remaining images cannot be used for pose estimation
    set_remainingViewId.clear();
    return false;
  }

  // Add the image view index that share the most of 2D-3D correspondences
  vec_possible_indexes.push_back(vec_putative[0].first);

  // Then, add all the image view indexes that have at least N% of the number of the matches of the best image.
  const IndexT M = vec_putative[0].second; // Number of 2D-3D correspondences
  const size_t threshold = std::max(static_cast<size_t>(dThresholdGroup * M), minPointsThreshold);
  for (size_t i = 1; i < vec_putative.size() &&
    vec_putative[i].second > threshold; ++i)
  {
    vec_possible_indexes.push_back(vec_putative[i].first);
  }
  std::cout << "FindImagesWithPossibleResection with " << vec_possible_indexes.size() << " images took: " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - chrono_start).count() << " msec" << std::endl;
  std::cout << "FindImagesWithPossibleResection: 2D-3D associations is between " << vec_putative.front().second << " and " << vec_putative[vec_possible_indexes.size()-1].second
            << " (threshold was " << threshold << ")" << std::endl;
  return true;
}

/**
 * @brief Add one image to the 3D reconstruction. To the resectioning of
 * the camera and triangulate all the new possible tracks.
 * @param[in] viewIndex: image index to add to the reconstruction.
 *
 * A. Compute 2D/3D matches
 * B. Look if intrinsic data is known or not
 * C. Do the resectioning: compute the camera pose.
 * D. Refine the pose of the found camera
 * E. Update the global scene with the new camera
 * F. Update the observations into the global scene structure
 * G. Triangulate new possible 2D tracks
 */
bool SequentialSfMReconstructionEngine::Resection(const size_t viewIndex)
{
  using namespace tracks;

  // A. Compute 2D/3D matches
  // A1. list tracks ids used by the view
  const openMVG::tracks::TracksSet& set_tracksIds = _map_tracksPerView.at(viewIndex);

  // A2. intersects the track list with the reconstructed
  std::set<size_t> reconstructed_trackId;
  std::transform(_sfm_data.GetLandmarks().begin(), _sfm_data.GetLandmarks().end(),
    std::inserter(reconstructed_trackId, reconstructed_trackId.begin()),
    stl::RetrieveKey());

  // Get the ids of the already reconstructed tracks
  std::set<size_t> set_trackIdForResection;
  std::set_intersection(set_tracksIds.begin(), set_tracksIds.end(),
    reconstructed_trackId.begin(),
    reconstructed_trackId.end(),
    std::inserter(set_trackIdForResection, set_trackIdForResection.begin()));

  if (set_trackIdForResection.empty())
  {
    // No match. The image has no connection with already reconstructed points.
    std::cout << std::endl
      << "-------------------------------" << "\n"
      << "-- Resection of camera index: " << viewIndex << "\n"
      << "-- Resection status: " << "FAILED" << "\n"
      << "-------------------------------" << std::endl;
    return false;
  }

  // Get back featId associated to a tracksID already reconstructed.
  // These 2D/3D associations will be used for the resection.
  std::vector<size_t> vec_featIdForResection;
  TracksUtilsMap::GetFeatIndexPerViewAndTrackId(_map_tracks,
    set_trackIdForResection,
    viewIndex,
    &vec_featIdForResection);

  // Localize the image inside the SfM reconstruction
  Image_Localizer_Match_Data resection_data;
  resection_data.pt2D.resize(2, set_trackIdForResection.size());
  resection_data.pt3D.resize(3, set_trackIdForResection.size());

  // B. Look if intrinsic data is known or not
  const View * view_I = _sfm_data.GetViews().at(viewIndex).get();
  std::shared_ptr<cameras::IntrinsicBase> optional_intrinsic (nullptr);
  if (_sfm_data.GetIntrinsics().count(view_I->id_intrinsic))
  {
    optional_intrinsic = _sfm_data.GetIntrinsics().at(view_I->id_intrinsic);
  }

  size_t cpt = 0;
  std::set<size_t>::const_iterator iterTrackId = set_trackIdForResection.begin();
  for (std::vector<size_t>::const_iterator iterfeatId = vec_featIdForResection.begin();
    iterfeatId != vec_featIdForResection.end();
    ++iterfeatId, ++iterTrackId, ++cpt)
  {
    resection_data.pt3D.col(cpt) = _sfm_data.GetLandmarks().at(*iterTrackId).X;
    resection_data.pt2D.col(cpt) = _features_provider->feats_per_view.at(viewIndex)[*iterfeatId].coords().cast<double>();
  }

  // C. Do the resectioning: compute the camera pose.
  std::cout << std::endl
    << "-------------------------------" << std::endl
    << "-- Robust Resection of view: " << viewIndex << std::endl;

  geometry::Pose3 pose;
  const bool bResection = sfm::SfM_Localizer::Localize
  (
    Pair(view_I->ui_width, view_I->ui_height),
    optional_intrinsic.get(),
    resection_data,
    pose
  );

  if (!_sLoggingFile.empty())
  {
    using namespace htmlDocument;
    ostringstream os;
    os << "Resection of Image index: <" << viewIndex << "> image: "
      << view_I->s_Img_path <<"<br> \n";
    _htmlDocStream->pushInfo(htmlMarkup("h1",os.str()));

    os.str("");
    os << std::endl
      << "-------------------------------" << "<br>"
      << "-- Robust Resection of camera index: <" << viewIndex << "> image: "
      <<  view_I->s_Img_path <<"<br>"
      << "-- Threshold: " << resection_data.error_max << "<br>"
      << "-- Resection status: " << (bResection ? "OK" : "FAILED") << "<br>"
      << "-- Nb points used for Resection: " << vec_featIdForResection.size() << "<br>"
      << "-- Nb points validated by robust estimation: " << resection_data.vec_inliers.size() << "<br>"
      << "-- % points validated: "
      << resection_data.vec_inliers.size()/static_cast<float>(vec_featIdForResection.size()) << "<br>"
      << "-------------------------------" << "<br>";
    _htmlDocStream->pushInfo(os.str());
  }

  if (!bResection)
    return false;

  // D. Refine the pose of the found camera.
  // We use a local scene with only the 3D points and the new camera.
  {
    const bool b_new_intrinsic = (optional_intrinsic == nullptr);
    // A valid pose has been found (try to refine it):
    // If no valid intrinsic as input:
    //  init a new one from the projection matrix decomposition
    // Else use the existing one and consider it as constant.
    if (b_new_intrinsic)
    {
      // setup a default camera model from the found projection matrix
      Mat3 K, R;
      Vec3 t;
      KRt_From_P(resection_data.projection_matrix, &K, &R, &t);

      const double focal = (K(0,0) + K(1,1))/2.0;
      const Vec2 principal_point(K(0,2), K(1,2));

      // Create the new camera intrinsic group
      switch (_camType)
      {
        case PINHOLE_CAMERA:
          optional_intrinsic =
            std::make_shared<Pinhole_Intrinsic>
            (view_I->ui_width, view_I->ui_height, focal, principal_point(0), principal_point(1));
        break;
        case PINHOLE_CAMERA_RADIAL1:
          optional_intrinsic =
            std::make_shared<Pinhole_Intrinsic_Radial_K1>
            (view_I->ui_width, view_I->ui_height, focal, principal_point(0), principal_point(1));
        break;
        case PINHOLE_CAMERA_RADIAL3:
          optional_intrinsic =
            std::make_shared<Pinhole_Intrinsic_Radial_K3>
            (view_I->ui_width, view_I->ui_height, focal, principal_point(0), principal_point(1));
        break;
        case PINHOLE_CAMERA_BROWN:
          optional_intrinsic =
            std::make_shared<Pinhole_Intrinsic_Brown_T2>
            (view_I->ui_width, view_I->ui_height, focal, principal_point(0), principal_point(1));
        break;
        case PINHOLE_CAMERA_FISHEYE:
            optional_intrinsic =
                std::make_shared<Pinhole_Intrinsic_Fisheye>
            (view_I->ui_width, view_I->ui_height, focal, principal_point(0), principal_point(1));
        break;
        default:
          std::cerr << "Try to create an unknown camera type." << std::endl;
          return false;
      }
    }
    if(!sfm::SfM_Localizer::RefinePose(
      optional_intrinsic.get(), pose,
      resection_data, true, b_new_intrinsic))
    {
      return false;
    }

    // E. Update the global scene with the new found camera pose, intrinsic (if not defined)
    if (b_new_intrinsic)
    {
      // Since the view have not yet an intrinsic group before, create a new one
      IndexT new_intrinsic_id = 0;
      if (!_sfm_data.GetIntrinsics().empty())
      {
        // Since some intrinsic Id already exists,
        //  we have to create a new unique identifier following the existing one
        std::set<IndexT> existing_intrinsicId;
          std::transform(_sfm_data.GetIntrinsics().begin(), _sfm_data.GetIntrinsics().end(),
          std::inserter(existing_intrinsicId, existing_intrinsicId.begin()),
          stl::RetrieveKey());
        new_intrinsic_id = (*existing_intrinsicId.rbegin())+1;
      }
      _sfm_data.views.at(viewIndex).get()->id_intrinsic = new_intrinsic_id;
      _sfm_data.intrinsics[new_intrinsic_id]= optional_intrinsic;
    }
    // Update the view pose
    _sfm_data.poses[view_I->id_pose] = pose;
    _map_ACThreshold.insert(std::make_pair(viewIndex, resection_data.error_max));
  }

  // F. Update the observations into the global scene structure
  // - Add the new 2D observations to the reconstructed tracks
  iterTrackId = set_trackIdForResection.begin();
  for (size_t i = 0; i < resection_data.pt2D.cols(); ++i, ++iterTrackId)
  {
    const Vec3 X = resection_data.pt3D.col(i);
    const Vec2 x = resection_data.pt2D.col(i);
    const Vec2 residual = optional_intrinsic->residual(pose, X, x);
    if (residual.norm() < resection_data.error_max &&
        pose.depth(X) > 0)
    {
      // Inlier, add the point to the reconstructed track
      _sfm_data.structure[*iterTrackId].obs[viewIndex] = Observation(x, vec_featIdForResection[i]);
    }
  }

  // G. Triangulate new possible 2D tracks
  // List tracks that share content with this view and add observations and new 3D track if required.
  {
    // For all reconstructed images look for common content in the tracks.
    const std::set<IndexT> valid_views = Get_Valid_Views(_sfm_data);
#ifdef OPENMVG_USE_OPENMP
    #pragma omp parallel for schedule(dynamic)
#endif
    for (std::size_t i = 0; i < valid_views.size(); ++i)
    {
      std::set<IndexT>::const_iterator iter = valid_views.begin();
      std::advance(iter, i);
      const IndexT indexI = *iter;

      // Ignore the current view
      if (indexI == viewIndex)
        continue;

      const size_t I = std::min((IndexT)viewIndex, indexI);
      const size_t J = std::max((IndexT)viewIndex, indexI);

      // Find track correspondences between I and J
      const std::set<size_t> set_viewIndex = { I,J };
      openMVG::tracks::STLMAPTracks map_tracksCommonIJ;
      TracksUtilsMap::GetTracksInImagesFast(set_viewIndex, _map_tracks, _map_tracksPerView, map_tracksCommonIJ);

      const View * view_I = _sfm_data.GetViews().at(I).get();
      const View * view_J = _sfm_data.GetViews().at(J).get();
      const IntrinsicBase * cam_I = _sfm_data.GetIntrinsics().at(view_I->id_intrinsic).get();
      const IntrinsicBase * cam_J = _sfm_data.GetIntrinsics().at(view_J->id_intrinsic).get();
      const Pose3 pose_I = _sfm_data.GetPoseOrDie(view_I);
      const Pose3 pose_J = _sfm_data.GetPoseOrDie(view_J);

      size_t new_putative_track = 0, new_added_track = 0, extented_track = 0;
      for (const std::pair< size_t, tracks::submapTrack >& trackIt : map_tracksCommonIJ)
      {
        const size_t trackId = trackIt.first;
        const tracks::submapTrack & track = trackIt.second;

        const Vec2 xI = _features_provider->feats_per_view.at(I)[track.at(I)].coords().cast<double>();
        const Vec2 xJ = _features_provider->feats_per_view.at(J)[track.at(J)].coords().cast<double>();

        // test if the track already exists in 3D
        if (_sfm_data.structure.count(trackId) != 0)
        {
          // 3D point triangulated before, only add image observation if needed
#ifdef OPENMVG_USE_OPENMP
          #pragma omp critical
#endif
          {
            Landmark & landmark = _sfm_data.structure[trackId];
            if (landmark.obs.count(I) == 0)
            {
              const Vec2 residual = cam_I->residual(pose_I, landmark.X, xI);
              if (pose_I.depth(landmark.X) > 0 && residual.norm() < std::max(4.0, _map_ACThreshold.at(I)))
              {
                landmark.obs[I] = Observation(xI, track.at(I));
                ++extented_track;
              }
            }
            if (landmark.obs.count(J) == 0)
            {
              const Vec2 residual = cam_J->residual(pose_J, landmark.X, xJ);
              if (pose_J.depth(landmark.X) > 0 && residual.norm() < std::max(4.0, _map_ACThreshold.at(J)))
              {
                landmark.obs[J] = Observation(xJ, track.at(J));
                ++extented_track;
              }
            }
          }
        }
        else
        {
          // A new 3D point must be added
#ifdef OPENMVG_USE_OPENMP
          #pragma omp critical
#endif
          {
            ++new_putative_track;
          }
          Vec3 X_euclidean = Vec3::Zero();
          const Vec2 xI_ud = cam_I->get_ud_pixel(xI);
          const Vec2 xJ_ud = cam_J->get_ud_pixel(xJ);
          const Mat34 P_I = cam_I->get_projective_equivalent(pose_I);
          const Mat34 P_J = cam_J->get_projective_equivalent(pose_J);
          TriangulateDLT(P_I, xI_ud, P_J, xJ_ud, &X_euclidean);
          // Check triangulation results
          //  - Check angle (small angle leads imprecise triangulation)
          //  - Check positive depth
          //  - Check residual values
          const double angle = AngleBetweenRay(pose_I, cam_I, pose_J, cam_J, xI, xJ);
          const Vec2 residual_I = cam_I->residual(pose_I, X_euclidean, xI);
          const Vec2 residual_J = cam_J->residual(pose_J, X_euclidean, xJ);
          if (angle > 2.0 &&
            pose_I.depth(X_euclidean) > 0 &&
            pose_J.depth(X_euclidean) > 0 &&
            residual_I.norm() < std::max(4.0, _map_ACThreshold.at(I)) &&
            residual_J.norm() < std::max(4.0, _map_ACThreshold.at(J)))
          {
#ifdef OPENMVG_USE_OPENMP
            #pragma omp critical
#endif
            {
              // Add a new track
              Landmark & landmark = _sfm_data.structure[trackId];
              landmark.X = X_euclidean;
              landmark.obs[I] = Observation(xI, track.at(I));
              landmark.obs[J] = Observation(xJ, track.at(J));
              ++new_added_track;
            } // critical
          } // 3D point is valid
        } // else (New 3D point)
      }// For all correspondences
//#ifdef OPENMVG_USE_OPENMP
//        #pragma omp critical
//#endif
//        if (!map_tracksCommonIJ.empty())
//        {
//          std::cout
//            << "--Triangulated 3D points [" << I << "-" << J << "]:"
//            << "\n\t#Track extented: " << extented_track
//            << "\n\t#Validated/#Possible: " << new_added_track << "/" << new_putative_track
//            << "\n\t#3DPoint for the entire scene: " << _sfm_data.GetLandmarks().size() << std::endl;
//        }
    }
  }
  return true;
}

/// Bundle adjustment to refine Structure; Motion and Intrinsics
bool SequentialSfMReconstructionEngine::BundleAdjustment()
{
  Bundle_Adjustment_Ceres::BA_options options;
  if (_sfm_data.GetPoses().size() > 100)
  {
    options._preconditioner_type = ceres::JACOBI;
    options._linear_solver_type = ceres::SPARSE_SCHUR;
  }
  else
  {
    options._linear_solver_type = ceres::DENSE_SCHUR;
  }
  Bundle_Adjustment_Ceres bundle_adjustment_obj(options);
  return bundle_adjustment_obj.Adjust(_sfm_data, true, true, !_bFixedIntrinsics);
}

/**
 * @brief Discard tracks with too large residual error
 *
 * Remove observation/tracks that have:
 *  - too large residual error
 *  - too small angular value
 *
 * @return True if more than 'count' outliers have been removed.
 */
size_t SequentialSfMReconstructionEngine::badTrackRejector(double dPrecision, size_t count)
{
  const size_t nbOutliers_residualErr = RemoveOutliers_PixelResidualError(_sfm_data, dPrecision, 2);
  const size_t nbOutliers_angleErr = RemoveOutliers_AngleError(_sfm_data, 2.0);

  std::cout << "badTrackRejector: nbOutliers_residualErr: " << nbOutliers_residualErr << ", nbOutliers_angleErr: " << nbOutliers_angleErr << "\n";
  return (nbOutliers_residualErr + nbOutliers_angleErr) > count;
}

} // namespace sfm
} // namespace openMVG

