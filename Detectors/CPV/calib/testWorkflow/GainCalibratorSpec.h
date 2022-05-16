// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "Framework/Task.h"
#include "Framework/ConfigParamRegistry.h"
#include "Framework/ControlService.h"
#include "Framework/WorkflowSpec.h"
#include "Framework/CCDBParamSpec.h"
#include "CCDB/CcdbApi.h"
#include "CCDB/CcdbObjectInfo.h"
#include "DetectorsCalibration/Utils.h"
#include "CPVCalibration/GainCalibrator.h"
#include "DataFormatsCPV/Digit.h"
#include "DataFormatsCPV/TriggerRecord.h"
#include "DetectorsBase/GRPGeomHelper.h"

using namespace o2::framework;

namespace o2
{
namespace calibration
{
class CPVGainCalibratorSpec : public o2::framework::Task
{
 public:
  CPVGainCalibratorSpec(std::shared_ptr<o2::base::GRPGeomRequest> req) : mCCDBRequest(req) {}

  //_________________________________________________________________
  void init(o2::framework::InitContext& ic) final
  {
    o2::base::GRPGeomHelper::instance().setRequest(mCCDBRequest);
    // auto slotL = ic.options().get<uint32_t>("tf-per-slot");
    // auto delay = ic.options().get<uint32_t>("max-delay");
    auto updateInterval = ic.options().get<uint32_t>("updateInterval"); // in TF
    bool updateAtTheEndOfRunOnly = ic.options().get<bool>("updateAtTheEndOfRunOnly");
    mCalibrator = std::make_unique<o2::cpv::GainCalibrator>();
    mCalibrator->setSlotLength(0);
    mCalibrator->setMaxSlotsDelay(1000);
    if (updateAtTheEndOfRunOnly) {
      mCalibrator->setUpdateAtTheEndOfRunOnly();
    }
    mCalibrator->setCheckIntervalInfiniteSlot(updateInterval);
    mCalibrator->setUpdateTFInterval(updateInterval);
    LOG(info) << "CPVGainCalibratorSpec initialized";
    LOG(info) << "tf-per-slot = 0 (inconfigurable for this calibrator)";
    LOG(info) << "max-delay = 1000 (inconfigurable for this calibrator)";
    LOG(info) << "updateInterval = " << updateInterval;
    LOG(info) << "updateAtTheEndOfRunOnly = " << updateAtTheEndOfRunOnly;
  }

  void finaliseCCDB(o2::framework::ConcreteDataMatcher& matcher, void* obj) final
  {
    o2::base::GRPGeomHelper::instance().finaliseCCDB(matcher, obj);
  }

  //_________________________________________________________________
  void run(o2::framework::ProcessingContext& pc) final
  {
    o2::base::GRPGeomHelper::instance().checkUpdates(pc);
    o2::base::TFIDInfoHelper::fillTFIDInfo(pc, mCalibrator->getCurrentTFInfo());
    TFType tfcounter = mCalibrator->getCurrentTFInfo().startTime;
    auto&& digits = pc.inputs().get<gsl::span<o2::cpv::Digit>>("calibdigs");

    // previous gains
    if (!mCalibrator->isSettedPreviousGains()) {
      const auto previousGains = o2::framework::DataRefUtils::as<CCDBSerialized<o2::cpv::CalibParams>>(pc.inputs().get("gains"));
      if (previousGains) {
        mCalibrator->setPreviousGains(new o2::cpv::CalibParams(*previousGains));
        LOG(info) << "GainCalibratorSpec()::run() : I got previous gains";
      }
    }

    // previous calib data
    if (!mCalibrator->isSettedPreviousGainCalibData()) {
      const auto previousGainCalibData = o2::framework::DataRefUtils::as<CCDBSerialized<o2::cpv::GainCalibData>>(pc.inputs().get("calibdata"));
      if (previousGainCalibData) {
        mCalibrator->setPreviousGainCalibData(new o2::cpv::GainCalibData(*previousGainCalibData));
        LOG(info) << "GainCalibratorSpec()::run() : I got previous GainCalibData: ";
        previousGainCalibData->print();
      }
    }

    // fill statistics
    LOG(info) << "Processing TF " << tfcounter << " with " << digits.size() << " digits";
    mCalibrator->process(digits); // fill TimeSlot with digits from 1 event and check slots for finalization

    // inform about results and send output to ccdb
    auto gainsInfoVecSize = mCalibrator->getCcdbInfoGainsVector().size();
    if (gainsInfoVecSize) {
      LOG(info) << "Created " << gainsInfoVecSize << " o2::cpv::CalibParams objects.";
      sendOutput(pc.outputs());
    }

    // and send results to CCDB (if any)
    if (gainsInfoVecSize) {
      sendOutput(pc.outputs());
    }
  }
  void endOfStream(o2::framework::EndOfStreamContext& ec) final
  {
    // prepare GainCalibData outputs and send it to ccdb
    mCalibrator->prepareForEnding();
    auto gainCDInfoVecSize = mCalibrator->getCcdbInfoGainCalibDataVector().size();
    if (gainCDInfoVecSize) {
      LOG(info) << "Created " << gainCDInfoVecSize << " o2::cpv::GainCalibData objects.";
      sendOutput(ec.outputs());
    }
  }
  //_________________________________________________________________
 private:
  std::unique_ptr<o2::cpv::GainCalibrator> mCalibrator;
  std::shared_ptr<o2::base::GRPGeomRequest> mCCDBRequest;
  //_________________________________________________________________
  void sendOutput(DataAllocator& output)
  {
    bool isSomethingSent = false;
    isSomethingSent = sendOutputWhat<o2::cpv::CalibParams>(mCalibrator->getGainsVector(), mCalibrator->getCcdbInfoGainsVector(), "CPV_Gains", output);
    isSomethingSent += sendOutputWhat<o2::cpv::GainCalibData>(mCalibrator->getGainCalibDataVector(), mCalibrator->getCcdbInfoGainCalibDataVector(), "CPV_GainCD", output);

    if (isSomethingSent) {
      mCalibrator->initOutput(); // reset the outputs once they are already sent
    }
  }

  template <class Payload>
  bool sendOutputWhat(const std::vector<Payload>& payloadVec, std::vector<o2::ccdb::CcdbObjectInfo>& infoVec, header::DataDescription what, DataAllocator& output)
  {
    assert(payloadVec.size() == infoVec.size());
    if (!payloadVec.size()) {
      return false;
    }

    for (uint32_t i = 0; i < payloadVec.size(); i++) {
      auto& w = infoVec[i];
      auto image = o2::ccdb::CcdbApi::createObjectImage(&payloadVec[i], &w);
      LOG(info) << "Sending object " << w.getPath() << "/" << w.getFileName() << " of size " << image->size()
                << " bytes, valid for " << w.getStartValidityTimestamp() << " : " << w.getEndValidityTimestamp();
      output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, what, i}, *image.get()); // vector<char>
      output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, what, i}, w);            // root-serialized
    }

    return true;
  }
  //_________________________________________________________________
}; // class CPVGainCalibratorSpec
} // namespace calibration

namespace framework
{
DataProcessorSpec getCPVGainCalibratorSpec()
{
  using device = o2::calibration::CPVGainCalibratorSpec;

  std::vector<InputSpec> inputs;
  inputs.emplace_back("calibdigs", "CPV", "CALIBDIGITS", 0, Lifetime::Timeframe);
  inputs.emplace_back("calibdata", "CPV", "CPV_GainCD", 0, Lifetime::Condition, ccdbParamSpec("CPV/PhysicsRun/GainCalibData"));
  inputs.emplace_back("gains", "CPV", "CPV_Gains", 0, Lifetime::Condition, ccdbParamSpec("CPV/Calib/Gains"));
  auto ccdbRequest = std::make_shared<o2::base::GRPGeomRequest>(true,                           // orbitResetTime
                                                                true,                           // GRPECS=true
                                                                false,                          // GRPLHCIF
                                                                false,                          // GRPMagField
                                                                false,                          // askMatLUT
                                                                o2::base::GRPGeomRequest::None, // geometry
                                                                inputs);
  std::vector<OutputSpec> outputs;
  // Length of data description ("CPV_Pedestals") must be < 16 characters.
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, "CPV_Gains"}, Lifetime::Sporadic);
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, "CPV_Gains"}, Lifetime::Sporadic);
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, "CPV_GainCD"}, Lifetime::Sporadic);
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, "CPV_GainCD"}, Lifetime::Sporadic);

  return DataProcessorSpec{
    "cpv-gain-calibration",
    inputs,
    outputs,
    AlgorithmSpec{adaptFromTask<device>(ccdbRequest)},
    Options{
      {"updateAtTheEndOfRunOnly", VariantType::Bool, false, {"finalize the slots and prepare the CCDB entries only at the end of the run."}},
      {"updateInterval", VariantType::UInt32, 100u, {"try to finalize the slot (and produce calibration) when the updateInterval has passed."}}}};
}
} // namespace framework
} // namespace o2
