/**
 * spaint: SLAMComponent.cpp
 * Copyright (c) Torr Vision Group, University of Oxford, 2016. All rights reserved.
 */

#include "pipelinecomponents/SLAMComponentWithScoreForest.h"

#include <algorithm>
#include <tuple>
#include <random>

#include <boost/timer/timer.hpp>
#include <opencv2/imgproc.hpp>

#include <omp.h>

#include <DatasetRGBDInfiniTAM.hpp>

#include <libalglib/optimization.h>

#include "ocv/OpenCVUtil.h"
#include "randomforest/cuda/GPUForest_CUDA.h"

#include "Helpers.hpp"

using namespace InputSource;
using namespace ITMLib;
using namespace ORUtils;
using namespace RelocLib;

#define ENABLE_TIMERS

namespace spaint
{

//#################### CONSTRUCTORS ####################

SLAMComponentWithScoreForest::SLAMComponentWithScoreForest(
    const SLAMContext_Ptr& context, const std::string& sceneID,
    const ImageSourceEngine_Ptr& imageSourceEngine, TrackerType trackerType,
    const std::vector<std::string>& trackerParams, MappingMode mappingMode,
    TrackingMode trackingMode) :
    SLAMComponent(context, sceneID, imageSourceEngine, trackerType,
        trackerParams, mappingMode, trackingMode)
{
  m_dataset.reset(
      new DatasetRGBDInfiniTAM(
          "/home/tcavallari/code/scoreforests/apps/TrainAndTest/SettingsDatasetRGBDInfiniTAMDesk.yml",
          "/media/data/", 5, 1.0, "DFBP", true, 0, false, 42));

  m_dataset->LoadForest();
//  m_dataset->ResetNodeAndLeaves();

  m_featureExtractor =
      FeatureCalculatorFactory::make_rgbd_patch_feature_calculator(
          ITMLib::ITMLibSettings::DEVICE_CUDA);
  m_featureImage.reset(new RGBDPatchFeatureImage(Vector2i(0, 0), true, true)); // Dummy size just to allocate the container
  m_leafImage.reset(
      new GPUForest::LeafIndicesImage(Vector2i(0, 0), true, true)); // Dummy size just to allocate the container
  m_predictionsImage.reset(
      new GPUForestPredictionsImage(Vector2i(0, 0), true, true)); // Dummy size just to allocate the container

  m_gpuForest.reset(new GPUForest_CUDA(*m_dataset->GetForest()));

//  m_gpuForest->reset_predictions();

  // Set params as in scoreforests
  m_kInitRansac = 1024;
  m_nbPointsForKabschBoostrap = 3;
  m_useAllModesPerLeafInPoseHypothesisGeneration = true;
  m_checkMinDistanceBetweenSampledModes = true;
  m_minDistanceBetweenSampledModes = 0.3f;
  m_checkRigidTransformationConstraint = false; // Speeds up a lot, was true in scoreforests
//  m_checkRigidTransformationConstraint = true;
  m_translationErrorMaxForCorrectPose = 0.05f;
  m_batchSizeRansac = 500;
  m_trimKinitAfterFirstEnergyComputation = 64;
//  m_poseUpdate = true; // original
  m_poseUpdate = false; // faster, might be OK
  m_usePredictionCovarianceForPoseOptimization = true; // original implementation
//  m_usePredictionCovarianceForPoseOptimization = false;

  // Additional stuff
  m_maxNbModesPerLeaf = 10; //5-10 seem to be enough
}

//#################### DESTRUCTOR ####################
SLAMComponentWithScoreForest::~SLAMComponentWithScoreForest()
{
}

//#################### PROTECTED MEMBER FUNCTIONS ####################

SLAMComponent::TrackingResult SLAMComponentWithScoreForest::process_relocalisation(
    TrackingResult trackingResult)
{
  const SLAMState_Ptr& slamState = m_context->get_slam_state(m_sceneID);
  const ITMShortImage_Ptr& inputRawDepthImage =
      slamState->get_input_raw_depth_image();
  const ITMFloatImage_Ptr inputDepthImage(
      new ITMFloatImage(slamState->get_view()->depth->noDims, true, true));
  inputDepthImage->SetFrom(slamState->get_view()->depth,
      ORUtils::MemoryBlock<float>::CUDA_TO_CUDA);

//  const ITMUChar4Image_Ptr& inputRGBImage = slamState->get_input_rgb_image();
  const ITMUChar4Image_Ptr inputRGBImage(
      new ITMUChar4Image(slamState->get_view()->rgb->noDims, true, true));
  inputRGBImage->SetFrom(slamState->get_view()->rgb,
      ORUtils::MemoryBlock<Vector4u>::CUDA_TO_CUDA);
  inputRGBImage->UpdateHostFromDevice();

  const TrackingState_Ptr& trackingState = slamState->get_tracking_state();

  const VoxelRenderState_Ptr& liveVoxelRenderState =
      slamState->get_live_voxel_render_state();
  const View_Ptr& view = slamState->get_view();
  const SpaintVoxelScene_Ptr& voxelScene = slamState->get_voxel_scene();

  const Vector4f depthIntrinsics =
      view->calib.intrinsics_d.projectionParamsSimple.all;

  if (trackingResult == TrackingResult::TRACKING_FAILED)
  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "relocalization, overall: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif

    if (m_lowLevelEngine->CountValidDepths(inputDepthImage.get())
        < std::max(m_nbPointsForKabschBoostrap, m_batchSizeRansac))
    {
      std::cout
          << "Number of valid depth pixels insufficient to perform relocalization."
          << std::endl;
      return trackingResult;
    }

    evaluate_forest(inputRGBImage, inputDepthImage, depthIntrinsics);
    boost::optional<PoseCandidate> pose_candidate = estimate_pose();

    if (pose_candidate)
    {
      std::cout << "The final pose is:" << pose_candidate->cameraPose
          << "\n and has " << pose_candidate->inliers.size() << " inliers."
          << std::endl;

      trackingState->pose_d->SetInvM(pose_candidate->cameraPose);

      const bool resetVisibleList = true;
      m_denseVoxelMapper->UpdateVisibleList(view.get(), trackingState.get(),
          voxelScene.get(), liveVoxelRenderState.get(), resetVisibleList);
      prepare_for_tracking(TRACK_VOXELS);
      m_trackingController->Track(trackingState.get(), view.get());
      trackingResult = trackingState->trackerResult;
    }
    else
    {
      std::cout << "Cannot estimate a pose candidate." << std::endl;
    }
  }

  return trackingResult;

//  return trackingResult;
//
//  // Create ensemble predictions
//  std::vector<boost::shared_ptr<EnsemblePrediction>> predictions(
//      m_leafImage->noDims.width);
//
//  int max_modes = -1;
//  int total_modes = 0;
//
//  {
//#ifdef ENABLE_TIMERS
//    boost::timer::auto_cpu_timer t(6,
//        "creating predictions from leaves: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//
//    const int *leafData = m_leafImage->GetData(MEMORYDEVICE_CPU);
//
//    // Create vectors of leaves
//    std::vector<std::vector<size_t>> leaves_indices(m_leafImage->noDims.width);
//
//    {
//#ifdef ENABLE_TIMERS
//      boost::timer::auto_cpu_timer t(6,
//          "creating leaves array: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
//      for (size_t prediction_idx = 0; prediction_idx < leaves_indices.size();
//          ++prediction_idx)
//      {
//        auto &tree_leaves = leaves_indices[prediction_idx];
//        tree_leaves.reserve(m_leafImage->noDims.height);
//        for (int tree_idx = 0; tree_idx < m_leafImage->noDims.height;
//            ++tree_idx)
//        {
//          tree_leaves.push_back(
//              leafData[tree_idx * m_leafImage->noDims.width + prediction_idx]);
//        }
//      }
//    }
//
//#pragma omp parallel for reduction(max:max_modes), reduction(+:total_modes)
//    for (size_t prediction_idx = 0; prediction_idx < leaves_indices.size();
//        ++prediction_idx)
//    {
//      predictions[prediction_idx] =
//          m_dataset->GetForest()->GetPredictionForLeaves(
//              leaves_indices[prediction_idx]);
//
//      if (predictions[prediction_idx])
//      {
//        int nbModes = ToEnsemblePredictionGaussianMean(
//            predictions[prediction_idx].get())->_modes.size();
//
//        if (nbModes > max_modes)
//        {
//          max_modes = nbModes;
//        }
//
//        total_modes += nbModes;
//      }
//    }
//  }
//
//  std::cout << "Max number of modes: " << max_modes << std::endl;
//  std::cout << "Total number of modes: " << total_modes << std::endl;

//  Vector2i px(84, 46);
//  int linear_px = px.y * m_featureImage->noDims.width + px.x;
//
//  std::vector<size_t> leaf_indices;
//
//  std::cout << "Leaves for pixel " << px << ": "; // << leafData[0 * m_leafImage->noDims.width + linear_px] << " "
//  for (int treeIdx = 0; treeIdx < m_leafImage->noDims.height; ++treeIdx)
//  {
//    leaf_indices.push_back(
//        leafData[treeIdx * m_leafImage->noDims.width + linear_px]);
//    std::cout << leaf_indices.back() << " ";
//  }
//
//  std::cout << std::endl;
//
//  // Get the corresponding ensemble prediction
//  boost::shared_ptr<EnsemblePrediction> prediction =
//      m_dataset->GetForest()->GetPredictionForLeaves(leaf_indices);

//  EnsemblePredictionGaussianMean* gm = ToEnsemblePredictionGaussianMean(
//      prediction.get());
//  std::cout << "The prediction has " << gm->_modes.size() << " modes.\n";
//  for (int mode_idx = 0; mode_idx < gm->_modes.size(); ++mode_idx)
//  {
//    std::cout << "Mode " << mode_idx << " has " << gm->_modes[mode_idx].size()
//        << " elements:\n";
//    for (int i = 0; i < gm->_modes[mode_idx].size(); ++i)
//    {
//      std::cout << "(" << gm->_modes[mode_idx][i]->_mean.transpose() << ") ";
//    }
//    std::cout << "\n";
//  }
//
//  std::cout << std::endl;

//  return trackingResult;

  if (trackingResult == TrackingResult::TRACKING_FAILED)
  {
    std::cout << "Tracking failed, trying to relocalize..." << std::endl;

    cv::Mat rgbd = build_rgbd_image(inputRGBImage, inputRawDepthImage);
//    boost::shared_ptr<EnsemblePredictionGaussianMean> prediction;

//    {
//      boost::timer::auto_cpu_timer t(6, "evaluating pixel: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//
//      boost::shared_ptr<InputOutputData> feature = m_dataset->ComputeFeaturesForPixel(rgbd, 100, 100);
//      prediction = boost::dynamic_pointer_cast<EnsemblePredictionGaussianMean>(m_dataset->PredictForFeature(feature));
//    }
//
//    return trackingResult;
//
//    std::cout << "Prediction has " << prediction->_modes.size() << " modes." << std::endl;
//
//    for(size_t mode = 0 ; mode < prediction->_modes.size(); ++mode)
//    {
//      std::cout << "Mode has " << prediction->_modes[mode].size() << " elements." << std::endl;
//      for(auto x : prediction->_modes[mode])
//      {
//        std::cout << "Npoints: " << x->_nbPoints << " - mean: " << x->_mean.transpose() << std::endl;
//      }
//    }

//    DatasetRGBD7Scenes::PredictionsCache cache;
//    std::vector<DatasetRGBD7Scenes::PoseCandidate> candidates;
//
//    {
//      boost::timer::auto_cpu_timer t(6, "generating candidates: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//      m_dataset->GeneratePoseCandidatesFromImage(rgbd, cache, candidates);
//    }
//
//    std::cout << "Cache has " << cache.size() << " entries. computed " << candidates.size() << " candidates." << std::endl;
//
//    std::mt19937 random_engine;
//
//    std::vector<std::pair<int, int>> sampled_pixels;
//
//    {
//      boost::timer::auto_cpu_timer t(6, "sampling candidates for ransac: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//      std::vector<bool> dummy_mask;
//      m_dataset->SamplePixelCandidatesForPoseUpdate(rgbd, cache, dummy_mask, sampled_pixels, random_engine);
//    }
//
//    std::cout << "Sampled " << sampled_pixels.size() << " pixels for RANSAC" << std::endl;

    DatasetRGBD7Scenes::PoseCandidate pose;

    {
      boost::timer::auto_cpu_timer t(6,
          "estimating pose: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
      pose = m_dataset->EstimatePose(rgbd);
    }

    std::cout << "new impl, pose:\n" << std::get < 0
        > (pose) << "\n" << std::endl;

//    // Now compute features
//    std::vector<boost::shared_ptr<InputOutputData>> featuresBuffer;
//
//    {
//      boost::timer::auto_cpu_timer t(6, "computing features: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//      m_dataset->ComputeFeaturesForImage(rgbd, featuresBuffer);
//    }
//
//    std::cout << "Computed " << featuresBuffer.size() << " features." << std::endl;
//
//    std::vector<EnsemblePrediction *> predictions;
//
//    // Evaluate forest
//    {
//      boost::timer::auto_cpu_timer t(6, "evaluating forest: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//      m_dataset->EvaluateForest(featuresBuffer, predictions);
//    }
//
//    std::cout << "Forest evaluated" << std::endl;
//
//    // Find pose
//    std::tuple<Eigen::MatrixXf, std::vector<std::pair<int, int>>, float, int> result;
//
//    {
//      boost::timer::auto_cpu_timer t(6, "estimating pose: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//      result = m_dataset->PoseFromPredictions(rgbd, featuresBuffer, predictions);
//    }
//
//    std::cout << "Pose estimated: " << std::get<0>(result) << "\nwith "<< std::get<1>(result).size() << " inliers." << std::endl;

    Matrix4f invPose;
    Eigen::Map<Eigen::Matrix4f> em(invPose.m);
    em = std::get < 0 > (pose);

    trackingState->pose_d->SetInvM(invPose);

    const bool resetVisibleList = true;
    m_denseVoxelMapper->UpdateVisibleList(view.get(), trackingState.get(),
        voxelScene.get(), liveVoxelRenderState.get(), resetVisibleList);
    prepare_for_tracking(TRACK_VOXELS);
    m_trackingController->Track(trackingState.get(), view.get());
    trackingResult = trackingState->trackerResult;

//    // cleanup
//    for(size_t i = 0; i < featuresBuffer.size(); ++i) delete featuresBuffer[i];
//    for(size_t i = 0; i < predictions.size(); ++i) delete predictions[i];
  }
//  else if (trackingResult == TrackingResult::TRACKING_GOOD)
//  {
//    cv::Matx44f invPose(trackingState->pose_d->GetInvM().m);
//    cv::Mat cvInvPose(invPose.t()); // Matrix4f is col major
//
//    cv::Mat rgbd = build_rgbd_image(inputRGBImage, inputRawDepthImage);
//
//    {
//      boost::timer::auto_cpu_timer t(6, "integrating new image: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//      m_dataset->AddImageFeaturesToForest(rgbd, cvInvPose);
//    }
//  }

  return trackingResult;
}

//#################### PROTECTED MEMBER FUNCTIONS ####################

void SLAMComponentWithScoreForest::evaluate_forest(
    const ITMUChar4Image_CPtr &inputRGBImage,
    const ITMFloatImage_CPtr &inputDepthImage, const Vector4f &depthIntrinsics)
{
  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "computing features on the GPU: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
    m_featureExtractor->ComputeFeature(inputRGBImage, inputDepthImage,
        depthIntrinsics, m_featureImage);
  }

  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "evaluating forest on the GPU: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
    m_gpuForest->evaluate_forest(m_featureImage, m_leafImage);
  }

  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "generating ensemble predictions on the GPU: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
    m_gpuForest->get_predictions(m_leafImage, m_predictionsImage);
  }
}

cv::Mat SLAMComponentWithScoreForest::build_rgbd_image(
    const ITMUChar4Image_Ptr &inputRGBImage,
    const ITMShortImage_Ptr &inputRawDepthImage) const
{
#ifdef ENABLE_TIMERS
  boost::timer::auto_cpu_timer t(6,
      "creating rgbd: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif

  // Create RGBD Mat wrappers to use in the forest
  cv::Mat rgb = OpenCVUtil::make_rgb_image(
      inputRGBImage->GetData(MemoryDeviceType::MEMORYDEVICE_CPU),
      inputRGBImage->noDims.width, inputRGBImage->noDims.height);
  cv::Mat depth(inputRawDepthImage->noDims.height,
      inputRawDepthImage->noDims.width, CV_16SC1,
      inputRawDepthImage->GetData(MemoryDeviceType::MEMORYDEVICE_CPU));

  // scoreforests wants rgb data
  cv::cvtColor(rgb, rgb, CV_BGR2RGB);

  // convert to float images
  rgb.convertTo(rgb, CV_32F);
  depth.convertTo(depth, CV_32F);

  // Dummy channel to fill the rgbd image
  cv::Mat dummyFiller = cv::Mat::zeros(inputRawDepthImage->noDims.height,
      inputRawDepthImage->noDims.width, CV_32FC1);

  std::vector<cv::Mat> channels;
  cv::split(rgb, channels);
  // swap r with b
  std::swap(channels[0], channels[2]);

  // insert 2 dummies and the depth
  channels.push_back(dummyFiller);
  channels.push_back(dummyFiller);
  channels.push_back(depth);
  channels.push_back(dummyFiller);
  channels.push_back(dummyFiller);
  channels.push_back(dummyFiller); // 9 channels since the sampling functions use Vec9f...

  cv::Mat rgbd;
  cv::merge(channels, rgbd);

  return rgbd;
}

void SLAMComponentWithScoreForest::generate_pose_candidates(
    std::vector<PoseCandidate> &poseCandidates)
{
  const int nbThreads = 12;

  poseCandidates.reserve(m_kInitRansac);

  std::vector<std::mt19937> engs(nbThreads);
  for (int i = 0; i < nbThreads; ++i)
  {
    engs[i].seed(static_cast<unsigned int>(i + 1));
  }

  omp_set_num_threads(nbThreads);

//  std::cout << "Generating pose candidates Kabsch" << std::endl;
#pragma omp parallel for
  for (size_t i = 0; i < m_kInitRansac; ++i)
  {
    int threadId = omp_get_thread_num();
    PoseCandidate candidate;

    if (hypothesize_pose(candidate, engs[threadId]))
    {
      if (!candidate.inliers.empty()) // Has some inliers
      {
        candidate.cameraId = i;

#pragma omp critical
        poseCandidates.emplace_back(std::move(candidate));
      }
    }
  }
}

bool SLAMComponentWithScoreForest::hypothesize_pose(PoseCandidate &res,
    std::mt19937 &eng)
{
  Eigen::MatrixXf worldPoints(3, m_nbPointsForKabschBoostrap);
  Eigen::MatrixXf localPoints(3, m_nbPointsForKabschBoostrap);

  std::uniform_int_distribution<int> col_index_generator(0,
      m_featureImage->noDims.width - 1);
  std::uniform_int_distribution<int> row_index_generator(0,
      m_featureImage->noDims.height - 1);

  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
      MEMORYDEVICE_CPU);
  const GPUForest::LeafIndices *leafData = m_leafImage->GetData(
      MEMORYDEVICE_CPU);
  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
      MEMORYDEVICE_CPU);

  bool foundIsometricMapping = false;
  const int maxIterationsOuter = 20;
  int iterationsOuter = 0;

  while (!foundIsometricMapping && iterationsOuter < maxIterationsOuter)
  {
    ++iterationsOuter;
    std::vector<std::tuple<int, int, int>> selectedPixelsAndModes;

    const int maxIterationsInner = 6000;
    int iterationsInner = 0;
    while (selectedPixelsAndModes.size() != m_nbPointsForKabschBoostrap
        && iterationsInner < maxIterationsInner)
    {
      ++iterationsInner;

      const int x = col_index_generator(eng);
      const int y = row_index_generator(eng);
      const int linearFeatureIdx = y * m_featureImage->noDims.width + x;
      const RGBDPatchFeature &selectedFeature =
          patchFeaturesData[linearFeatureIdx];

      if (selectedFeature.position.w < 0.f) // Invalid feature
        continue;

      const GPUForestPrediction &selectedPrediction =
          predictionsData[linearFeatureIdx];

      if (selectedPrediction.nbModes == 0)
        continue;

      int selectedModeIdx = 0;
      if (m_useAllModesPerLeafInPoseHypothesisGeneration)
      {
        std::uniform_int_distribution<int> mode_generator(0,
            selectedPrediction.nbModes - 1);
        selectedModeIdx = mode_generator(eng);
      }

      // This is the first pixel, check that the pixel colour corresponds with the selected mode
      if (selectedPixelsAndModes.empty())
      {
        const Vector3u colourDiff = selectedFeature.colour.toVector3().toUChar()
            - selectedPrediction.modes[selectedModeIdx].colour;
        const bool consistentColour = abs(colourDiff.x) <= 30
            && abs(colourDiff.y) <= 30 && abs(colourDiff.z) <= 30;

        if (!consistentColour)
          continue;
      }

      // if (false)
      if (m_checkMinDistanceBetweenSampledModes)
      {
        const Vector3f worldPt =
            selectedPrediction.modes[selectedModeIdx].position;

        // Check that this mode is far enough from the other modes
        bool farEnough = true;

        for (size_t idxOther = 0; idxOther < selectedPixelsAndModes.size();
            ++idxOther)
        {
          int xOther, yOther, modeIdxOther;
          std::tie(xOther, yOther, modeIdxOther) =
              selectedPixelsAndModes[idxOther];

          const int linearIdxOther = yOther * m_featureImage->noDims.width
              + xOther;
          const GPUForestPrediction &predOther = predictionsData[linearIdxOther];

          Vector3f worldPtOther = predOther.modes[modeIdxOther].position;

          float distOther = length(worldPtOther - worldPt);
          if (distOther < m_minDistanceBetweenSampledModes)
          {
            farEnough = false;
            break;
          }
        }

        if (!farEnough)
          continue;
      }

      // isometry?
//       if (false)
      // if (true)
      if (m_checkRigidTransformationConstraint)
      {
        bool violatesConditions = false;

        for (int m = 0;
            m < selectedPixelsAndModes.size() && !violatesConditions; ++m)
        {
          int xFirst, yFirst, modeIdxFirst;
          std::tie(xFirst, yFirst, modeIdxFirst) = selectedPixelsAndModes[m];

          const int linearIdxOther = yFirst * m_featureImage->noDims.width
              + xFirst;
          const GPUForestPrediction &predFirst = predictionsData[linearIdxOther];

          const Vector3f worldPtFirst = predFirst.modes[modeIdxFirst].position;
          const Vector3f worldPtCur =
              selectedPrediction.modes[selectedModeIdx].position;

          float distWorld = length(worldPtFirst - worldPtCur);

          const Vector3f localPred =
              patchFeaturesData[linearIdxOther].position.toVector3();
          const Vector3f localCur = selectedFeature.position.toVector3();

          float distLocal = length(localPred - localCur);

          if (distLocal < m_minDistanceBetweenSampledModes)
            violatesConditions = true;

          if (std::abs(distLocal - distWorld)
              > 0.5f * m_translationErrorMaxForCorrectPose)
          {
            violatesConditions = true;
          }
        }

        if (violatesConditions)
          continue;
      }

      selectedPixelsAndModes.push_back(
          std::tuple<int, int, int>(x, y, selectedModeIdx));
//      iterationsInner = 0;
    }

//    std::cout << "Inner iterations: " << iterationsInner << std::endl;

    // Reached limit of iterations
    if (selectedPixelsAndModes.size() != m_nbPointsForKabschBoostrap)
      return false;

    std::vector<std::pair<int, int>> tmpInliers;
    for (size_t s = 0; s < selectedPixelsAndModes.size(); ++s)
    {
      int x, y, modeIdx;
      std::tie(x, y, modeIdx) = selectedPixelsAndModes[s];
      const int linearIdx = y * m_featureImage->noDims.width + x;
      const GPUForestPrediction &pred = predictionsData[linearIdx];

      Eigen::VectorXf localPt = Eigen::Map<const Eigen::Vector3f>(
          patchFeaturesData[linearIdx].position.v);

      Eigen::VectorXf worldPt = Eigen::Map<const Eigen::Vector3f>(
          pred.modes[modeIdx].position.v);

      for (int idx = 0; idx < 3; ++idx)
      {
        localPoints(idx, s) = localPt(idx);
        worldPoints(idx, s) = worldPt(idx);
      }

      tmpInliers.push_back(std::pair<int, int>(linearIdx, modeIdx));
    }

    {
//#ifdef ENABLE_TIMERS
//      boost::timer::auto_cpu_timer t(6,
//          "kabsch: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
//#endif
      Eigen::Map<Eigen::Matrix4f>(res.cameraPose.m) = Helpers::Kabsch(
          localPoints, worldPoints);
    }

    foundIsometricMapping = true;

    res.inliers = tmpInliers;
    res.energy = 0.f;
    res.cameraId = -1;
  }

  if (iterationsOuter < maxIterationsOuter)
    return true;

  return false;
}

boost::optional<SLAMComponentWithScoreForest::PoseCandidate> SLAMComponentWithScoreForest::estimate_pose()
{
  std::mt19937 random_engine;

  m_featureImage->UpdateHostFromDevice(); // Need the features on the host for now
  m_predictionsImage->UpdateHostFromDevice();

  // Generate pose candidates with the new implementation
  std::vector<PoseCandidate> candidates;

  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "generating initial candidates: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
    generate_pose_candidates(candidates);
  }

  std::cout << "Generated " << candidates.size() << " initial candidates."
      << std::endl;

  if (m_trimKinitAfterFirstEnergyComputation < candidates.size())
  {
#ifdef ENABLE_TIMERS
    boost::timer::auto_cpu_timer t(6,
        "first trim: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
    int nbSamplesPerCamera = candidates[0].inliers.size();
    std::vector<std::pair<int, int>> sampledPixelIdx;
    std::vector<bool> dummy_vector;

    {
#ifdef ENABLE_TIMERS
      boost::timer::auto_cpu_timer t(6,
          "sample pixels: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
      sample_pixels_for_ransac(dummy_vector, sampledPixelIdx, random_engine,
          m_batchSizeRansac);
    }

    {
#ifdef ENABLE_TIMERS
      boost::timer::auto_cpu_timer t(6,
          "update inliers: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
      update_inliers_for_optimization(sampledPixelIdx, candidates);
    }

    {
#ifdef ENABLE_TIMERS
      boost::timer::auto_cpu_timer t(6,
          "compute and sort energies: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif
      compute_and_sort_energies(candidates);
    }

    candidates.erase(
        candidates.begin() + m_trimKinitAfterFirstEnergyComputation,
        candidates.end());

    if (m_trimKinitAfterFirstEnergyComputation > 1)
    {
      for (size_t p = 0; p < candidates.size(); ++p)
      {
        std::vector<std::pair<int, int>> &samples = candidates[p].inliers;
        if (samples.size() > nbSamplesPerCamera)
          samples.erase(samples.begin() + nbSamplesPerCamera, samples.end());
      }
    }
  }

  //  std::cout << candidates.size() << " candidates remaining." << std::endl;
  //  std::cout << "Premptive RANSAC" << std::endl;

#ifdef ENABLE_TIMERS
  boost::timer::auto_cpu_timer t(6,
      "ransac: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
#endif

  std::vector<bool> maskSampledPixels(m_featureImage->dataSize, false);

  float iteration = 0.0f;

  while (candidates.size() > 1)
  {
    //    boost::timer::auto_cpu_timer t(
    //        6, "ransac iteration: %ws wall, %us user + %ss system = %ts CPU (%p%)\n");
    ++iteration;
    //    std::cout << candidates.size() << " camera remaining" << std::endl;

    std::vector<std::pair<int, int>> sampledPixelIdx;
    sample_pixels_for_ransac(maskSampledPixels, sampledPixelIdx, random_engine,
        m_batchSizeRansac);

    //    std::cout << "Updating inliers to each pose candidate..." << std::endl;
    update_inliers_for_optimization(sampledPixelIdx, candidates);

    if (m_poseUpdate)
    {
      update_candidate_poses(candidates);
    }

    compute_and_sort_energies(candidates);

    // Remove half of the candidates with the worse energies
    candidates.erase(candidates.begin() + candidates.size() / 2,
        candidates.end());
  }

  return !candidates.empty() ? candidates[0] : boost::optional<PoseCandidate>();
}

void SLAMComponentWithScoreForest::sample_pixels_for_ransac(
    std::vector<bool> &maskSampledPixels,
    std::vector<std::pair<int, int>> &sampledPixelIdx, std::mt19937 &eng,
    int batchSize)
{
  std::uniform_int_distribution<int> col_index_generator(0,
      m_featureImage->noDims.width - 1);
  std::uniform_int_distribution<int> row_index_generator(0,
      m_featureImage->noDims.height - 1);

  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
      MEMORYDEVICE_CPU);
  const GPUForest::LeafIndices *leafData = m_leafImage->GetData(
      MEMORYDEVICE_CPU);
  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
      MEMORYDEVICE_CPU);

  for (int i = 0; i < batchSize; ++i)
  {
    bool validIndex = false;
    int innerIterations = 0;

    while (!validIndex && innerIterations++ < 50)
    {
      std::pair<int, int> s;

      s.first = col_index_generator(eng);
      s.second = row_index_generator(eng);

      const int linearIdx = s.second * m_featureImage->noDims.width + s.first;

      if (patchFeaturesData[linearIdx].position.w >= 0.f)
      {
        const GPUForestPrediction &selectedPrediction =
            predictionsData[linearIdx];

        if (selectedPrediction.nbModes > 0)
        {
          validIndex = maskSampledPixels.empty()
              || !maskSampledPixels[linearIdx];

          if (validIndex)
          {
            sampledPixelIdx.push_back(s);

            if (!maskSampledPixels.empty())
              maskSampledPixels[linearIdx] = true;
          }
        }
      }
    }

    if (!validIndex)
    {
      std::cout << "Couldn't sample a valid pixel. Returning "
          << sampledPixelIdx.size() << "/" << batchSize << std::endl;
      break;
    }
  }
}

void SLAMComponentWithScoreForest::update_inliers_for_optimization(
    const std::vector<std::pair<int, int>> &sampledPixelIdx,
    std::vector<PoseCandidate> &poseCandidates) const
{
#pragma omp parallel for
  for (size_t p = 0; p < poseCandidates.size(); ++p)
  {
    std::vector<std::pair<int, int>> &inliers = poseCandidates[p].inliers;

    // add all the samples as inliers
    for (size_t s = 0; s < sampledPixelIdx.size(); ++s)
    {
      int x = sampledPixelIdx[s].first;
      int y = sampledPixelIdx[s].second;
      inliers.push_back(
          std::pair<int, int>(y * m_featureImage->noDims.width + x, -1));
    }
  }
}

void SLAMComponentWithScoreForest::compute_and_sort_energies(
    std::vector<PoseCandidate> &poseCandidates) const
{
//  int nbPoseProcessed = 0;
#pragma omp parallel for
  for (size_t p = 0; p < poseCandidates.size(); ++p)
  {
    //#pragma omp critical
    //    {
    //      //#pragma omp flush(nbPoseProcessed)
    //      //      Helpers::displayPercentage(nbPoseProcessed++, poseCandidates.size());
    //    }

    PoseCandidate &candidate = poseCandidates[p];
    candidate.energy = compute_pose_energy(candidate.cameraPose,
        candidate.inliers);
  }

  // Sort by ascending energy
  std::sort(poseCandidates.begin(), poseCandidates.end(),
      [] (const PoseCandidate &a, const PoseCandidate &b)
      { return a.energy < b.energy;});
}

float SLAMComponentWithScoreForest::compute_pose_energy(
    const Matrix4f &candidateCameraPose,
    const std::vector<std::pair<int, int>> &inliersIndices) const
{
  float totalEnergy = 0.0f;

  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
      MEMORYDEVICE_CPU);
  const GPUForestPrediction *predictionsData = m_predictionsImage->GetData(
      MEMORYDEVICE_CPU);

  for (size_t s = 0; s < inliersIndices.size(); ++s)
  {
    const int linearIdx = inliersIndices[s].first;
    const Vector3f localPixel =
        patchFeaturesData[linearIdx].position.toVector3();
    const Vector3f projectedPixel = candidateCameraPose * localPixel;

    const GPUForestPrediction &pred = predictionsData[linearIdx];

    // eval individual energy
    float energy;
    int argmax = pred.get_best_mode_and_energy(projectedPixel, energy);

    // Has at least a valid mode
    if (argmax < 0)
    {
      // should have not been inserted in the inlier set
      throw std::runtime_error("prediction has no valid modes");
    }

    if (pred.modes[argmax].nbInliers == 0)
    {
      // the original implementation had a simple continue
      throw std::runtime_error("mode has no inliers");
    }

    energy /= static_cast<float>(pred.nbModes);
    energy /= static_cast<float>(pred.modes[argmax].nbInliers);

    if (energy < 1e-6f)
      energy = 1e-6f;
    totalEnergy += -log10f(energy);
  }

  return totalEnergy / static_cast<float>(inliersIndices.size());
}

void SLAMComponentWithScoreForest::update_candidate_poses(
    std::vector<PoseCandidate> &poseCandidates) const
{
//  int nbUpdated = 0;
#pragma omp parallel for
  for (size_t i = 0; i < poseCandidates.size(); ++i)
  {
    if (update_candidate_pose(poseCandidates[i]))
    {
      //#pragma omp atomic
      //      ++nbUpdated;
    }
  }
  //  std::cout << nbUpdated << "/" << poseCandidates.size() << " updated cameras" << std::endl;
}

namespace
{
struct PointsForLM
{
  PointsForLM(int nbPts) :
      pts(nbPts), blurred_img(NULL)
  {
  }
  ~PointsForLM()
  {
  }
  std::vector<
      std::pair<std::vector<Eigen::VectorXd>,
          std::vector<PredictedGaussianMean *>>> pts;
  GaussianAggregatedRGBImage *blurred_img;
};

static double EnergyForContinuous3DOptimizationUsingFullCovariance(
    std::vector<
        std::pair<std::vector<Eigen::VectorXd>,
            std::vector<PredictedGaussianMean *>>> &pts,
    Eigen::MatrixXd &candidateCameraPoseD)
{
  double res = 0.0;
  Eigen::VectorXd diff = Eigen::VectorXd::Zero(3);
  Eigen::VectorXd transformedPthomogeneous(4);

  for (int i = 0; i < pts.size(); ++i)
  {
    Helpers::Rigid3DTransformation(candidateCameraPoseD, pts[i].first[0],
        transformedPthomogeneous);

    for (int p = 0; p < 3; ++p)
    {
      diff(p) = transformedPthomogeneous(p) - pts[i].second[0]->_mean(p);
    }

    double err = sqrt(
        Helpers::MahalanobisSquared3x3(pts[i].second[0]->_inverseCovariance,
            diff));
    // double err = (Helpers::MahalanobisSquared3x3(pts[i].second[0]->_inverseCovariance, diff));
    res += err;
  }
  return res;
}

static void Continuous3DOptimizationUsingFullCovariance(
    const alglib::real_1d_array &x, alglib::real_1d_array &fi, void *ptr)
{
  PointsForLM *ptsLM = reinterpret_cast<PointsForLM *>(ptr);

  std::vector<
      std::pair<std::vector<Eigen::VectorXd>,
          std::vector<PredictedGaussianMean *>>> &pts = ptsLM->pts;
  // integrate the size of the clusters?
  Eigen::VectorXd ksi(6);
  memcpy(ksi.data(), x.getcontent(), 6 * sizeof(double));
  /*for (int i = 0 ; i < 6 ; ++i)
   {
   ksi(i) = x[i];
   }*/
  Eigen::MatrixXd updatedCandidateCameraPoseD =
      Helpers::LieAlgebraToLieGroupSE3(ksi);

  fi[0] = EnergyForContinuous3DOptimizationUsingFullCovariance(pts,
      updatedCandidateCameraPoseD);
  return;
}

/***************************************************/
/* Routines to optimize the sum of 3D L2 distances */
/***************************************************/

static double EnergyForContinuous3DOptimizationUsingL2(
    std::vector<
        std::pair<std::vector<Eigen::VectorXd>,
            std::vector<PredictedGaussianMean *>>> &pts,
    Eigen::MatrixXd &candidateCameraPoseD)
{
  double res = 0.0;
  Eigen::VectorXd diff = Eigen::VectorXd::Zero(3);
  Eigen::VectorXd transformedPthomogeneous(4);

  for (int i = 0; i < pts.size(); ++i)
  {
    Helpers::Rigid3DTransformation(candidateCameraPoseD, pts[i].first[0],
        transformedPthomogeneous);

    for (int p = 0; p < 3; ++p)
    {
      diff(p) = transformedPthomogeneous(p) - pts[i].second[0]->_mean(p);
    }

    double err = diff.norm();
    err *= err;
    res += err;
  }
  return res;
}

static void Continuous3DOptimizationUsingL2(const alglib::real_1d_array &x,
    alglib::real_1d_array &fi, void *ptr)
{
  PointsForLM *ptsLM = reinterpret_cast<PointsForLM *>(ptr);

  std::vector<
      std::pair<std::vector<Eigen::VectorXd>,
          std::vector<PredictedGaussianMean *>>> &pts = ptsLM->pts;
  // integrate the size of the clusters?
  Eigen::VectorXd ksi(6);
  memcpy(ksi.data(), x.getcontent(), 6 * sizeof(double));
  /*for (int i = 0 ; i < 6 ; ++i)
   {
   ksi(i) = x[i];
   }*/
  Eigen::MatrixXd updatedCandidateCameraPoseD =
      Helpers::LieAlgebraToLieGroupSE3(ksi);

  fi[0] = EnergyForContinuous3DOptimizationUsingL2(pts,
      updatedCandidateCameraPoseD);
  return;
}

static void call_after_each_step(const alglib::real_1d_array &x, double func,
    void *ptr)
{
  return;
}
}

bool SLAMComponentWithScoreForest::update_candidate_pose(
    PoseCandidate &poseCandidate) const
{
  throw std::runtime_error("not updated yet");

//  Eigen::Matrix4f &candidateCameraPose = std::get < 0 > (poseCandidate);
//  std::vector<std::pair<int, int>> &samples = std::get < 1 > (poseCandidate);
//
//  const RGBDPatchFeature *patchFeaturesData = m_featureImage->GetData(
//      MEMORYDEVICE_CPU);
//
//  PointsForLM ptsForLM(0);
//
//  for (int s = 0; s < samples.size(); ++s)
//  {
//    const int x = samples[s].first % m_featureImage->noDims.width;
//    const int y = samples[s].first / m_featureImage->noDims.width;
//    const int linearizedIdx = samples[s].first;
//
//    std::pair<std::vector<Eigen::VectorXd>, std::vector<PredictedGaussianMean *>> pt;
//
//    Eigen::VectorXf pixelLocalCoordinates = Eigen::Map<const Eigen::Vector4f>(
//        patchFeaturesData[linearizedIdx].position.v);
//
//    pt.first.push_back(pixelLocalCoordinates.cast<double>());
//    // Eigen::VectorXf  projectedPixel = candidateCameraPose * pixelLocalCoordinates;
//    Eigen::VectorXd projectedPixel = (candidateCameraPose
//        * pixelLocalCoordinates).cast<double>();
//
//    boost::shared_ptr<EnsemblePredictionGaussianMean> epgm =
//        m_featurePredictions[linearizedIdx];
//
//    int argmax = epgm->GetArgMax3D(projectedPixel, 0);
//    if (argmax == -1)
//      continue;
//    pt.second.push_back(epgm->_modes[argmax][0]);
//
//    if ((epgm->_modes[argmax][0]->_mean
//        - Helpers::ConvertWorldCoordinatesFromHomogeneousCoordinates(
//            projectedPixel)).norm() < 0.2)
//      ptsForLM.pts.push_back(pt);
//  }
//
//  // Continuous optimization
//  if (ptsForLM.pts.size() > 3)
//  {
//    alglib::real_1d_array ksi_;
//    double ksiD[6];
//    Eigen::MatrixXd candidateCameraPoseD = candidateCameraPose.cast<double>();
//
//    Eigen::VectorXd ksivd = Helpers::LieGroupToLieAlgebraSE3(
//        candidateCameraPoseD);
//
//    for (int i = 0; i < 6; ++i)
//    {
//      ksiD[i] = ksivd(i);
//    }
//
//    ksi_.setcontent(6, ksiD);
//
//    alglib::minlmstate state;
//    alglib::minlmreport rep;
//
//    double differentiationStep = 0.0001;
//    alglib::minlmcreatev(6, 1, ksi_, differentiationStep, state);
//
//    double epsg = 0.000001;
//    double epsf = 0;
//    double epsx = 0;
//    alglib::ae_int_t maxits = 100;
//    alglib::minlmsetcond(state, epsg, epsf, epsx, maxits);
//
//    double energyBefore, energyAfter;
//    if (m_usePredictionCovarianceForPoseOptimization)
//    {
//      energyBefore = EnergyForContinuous3DOptimizationUsingFullCovariance(
//          ptsForLM.pts, candidateCameraPoseD);
//      alglib::minlmoptimize(state, Continuous3DOptimizationUsingFullCovariance,
//          call_after_each_step, &ptsForLM);
//    }
//    else
//    {
//      energyBefore = EnergyForContinuous3DOptimizationUsingL2(ptsForLM.pts,
//          candidateCameraPoseD);
//      alglib::minlmoptimize(state, Continuous3DOptimizationUsingL2,
//          call_after_each_step, &ptsForLM);
//    }
//    alglib::minlmresults(state, ksi_, rep);
//
//    memcpy(ksiD, ksi_.getcontent(), sizeof(double) * 6);
//    for (int i = 0; i < 6; ++i)
//    {
//      ksivd(i) = ksiD[i];
//    }
//    Eigen::MatrixXd updatedCandidateCameraPoseD =
//        Helpers::LieAlgebraToLieGroupSE3(ksivd);
//
//    if (m_usePredictionCovarianceForPoseOptimization)
//    {
//      energyAfter = EnergyForContinuous3DOptimizationUsingFullCovariance(
//          ptsForLM.pts, updatedCandidateCameraPoseD);
//    }
//    else
//    {
//      energyAfter = EnergyForContinuous3DOptimizationUsingL2(ptsForLM.pts,
//          updatedCandidateCameraPoseD);
//    }
//
//    if (energyAfter < energyBefore)
//    {
//      candidateCameraPose = updatedCandidateCameraPoseD.cast<float>();
//      return true;
//    }
//  }
//
//  return false;
}

}