#ifndef SIOVCHANNELFILTERSERVICE_CC
#define SIOVCHANNELFILTERSERVICE_CC

#include "art/Framework/Services/Registry/ServiceMacros.h"
#include "art/Framework/Services/Registry/ActivityRegistry.h"
#include "art/Framework/Principal/Event.h"
#include "fhiclcpp/ParameterSet.h"
#include "CalibrationDBI/Interface/IChannelFilterService.h"
#include "CalibrationDBI/Providers/SIOVChannelFilterProvider.h"
#include "art/Framework/Services/Registry/ServiceHandle.h" 
#include "Geometry/Geometry.h"

#include "RawData/RawDigit.h"
#include "RawData/raw.h"
#include "Utilities/DetectorProperties.h"

namespace lariov{

  /**
     \class SIOVChannelFilterService
     art service implementation of IChannelFilterService.  Implements 
     a channel status retrieval service for database scheme in which 
     all elements in a database folder share a common interval of validity
  */
  class SIOVChannelFilterService : public IChannelFilterService {
  
    public:
    
      SIOVChannelFilterService(fhicl::ParameterSet const& pset, art::ActivityRegistry& reg);
      ~SIOVChannelFilterService(){}
      
      void PreProcessEvent(const art::Event& evt); 
     
    private:
    
      const IChannelFilterProvider& DoGetFilter() const override {
        return fProvider;
      }    
      
      const IChannelFilterProvider* DoGetFilterPtr() const override {
        return &fProvider;
      }
      
      void FindNoisyChannels(const art::Event& evt);
    
      SIOVChannelFilterProvider fProvider;
      bool                fFindNoisyChannels;     ///< Find noisy channels using raw digits
      std::string         fDigitModuleLabel;      ///< The full collection of hits for finding noisy channels
      float               fTruncMeanFraction;     ///< Fraction for truncated mean
      std::vector<double> fRmsCut;       ///< channel upper rms cut
  };
}//end namespace lariov
      
DECLARE_ART_SERVICE_INTERFACE_IMPL(lariov::SIOVChannelFilterService, lariov::IChannelFilterService, LEGACY)
      

namespace lariov{

  SIOVChannelFilterService::SIOVChannelFilterService(fhicl::ParameterSet const& pset, art::ActivityRegistry& reg) 
  : fProvider(pset.get<fhicl::ParameterSet>("ChannelFilterProvider"))
  {
    
    fFindNoisyChannels  = pset.get<bool>                ("FindNoisyChannels",   false);
    fDigitModuleLabel   = pset.get<std::string>         ("DigitModuleLabel",    "daq");
    fTruncMeanFraction  = pset.get<float>               ("TruncMeanFraction",   0.1);
    fRmsCut             = pset.get<std::vector<double> >("RMSRejectionCut",     std::vector<double>() = { 5.0, 5.0, 3.0});
    
    //register callback to update local database cache before each event is processed
    //reg.sPreProcessEvent.watch(&SIOVChannelFilterService::PreProcessEvent, *this);
    reg.sPreProcessEvent.watch(this, &SIOVChannelFilterService::PreProcessEvent);
    
  }
  
  
  void SIOVChannelFilterService::PreProcessEvent(const art::Event& evt) {
    
    //First grab an update from the database
    fProvider.Update(evt.time().value());

    //Update noisy channels using raw digits
    if (fFindNoisyChannels) this->FindNoisyChannels(evt);
  } 
  
  
  void SIOVChannelFilterService::FindNoisyChannels( art::Event const& evt ) {
     
     // Read in the digit List object(s).
    art::Handle< std::vector<raw::RawDigit> > digitVecHandle;
    evt.getByLabel(fDigitModuleLabel, digitVecHandle);

    // Require a valid handle
    if (!digitVecHandle.isValid()) return;

    art::ServiceHandle<geo::Geometry > geo;
    art::ServiceHandle<util::DetectorProperties> detectorProperties;
    unsigned int maxTimeSamples = detectorProperties->NumberTimeSamples();

    // Loop over raw digits, calculate the baseline rms of each one, and 
    // declare a channel noisy if its rms is above user-defined threshold 
    for(size_t rdIter = 0; rdIter < digitVecHandle->size(); ++rdIter) {


      // get the reference to the current raw::RawDigit and check that it isn't already dead or disconnected
      art::Ptr<raw::RawDigit> digitVec(digitVecHandle, rdIter);
      raw::ChannelID_t channel = digitVec->Channel();
      if (fProvider.IsBad(channel) || !fProvider.IsPresent(channel)) continue;

      unsigned int dataSize = digitVec->Samples();
      maxTimeSamples = std::min(maxTimeSamples, dataSize);


      // vector holding uncompressed adc values
      std::vector<short> rawadc;
      rawadc.resize(maxTimeSamples);
      raw::Uncompress(digitVec->ADCs(), rawadc, digitVec->Compression());


      // The strategy for finding the average for a given wire will be to
      // find the most populated bin and the average using the neighboring bins
      // To do this we'll use a map with key the bin number and data the count in that bin
      // Define the map first
      std::map<short,short> binAdcMap;

      // Populate the map
      for(const auto& adcVal : rawadc)
      {
        binAdcMap[adcVal]++;
      }

      // Find the max bin
      short binMax(-1);
      short binMaxCnt(0);
      for(const auto& binAdcItr : binAdcMap)
      {
        if (binAdcItr.second > binMaxCnt)
        {
          binMax    = binAdcItr.first;
          binMaxCnt = binAdcItr.second;
        }
      }


      // Armed with the max bin and its count, now set up to get an average
      // about this bin. We'll want to cut off at some user defined fraction
      // of the total bins on the wire
      int minNumBins = (1. - fTruncMeanFraction) * dataSize - 1;
      int curBinCnt(binMaxCnt);

      double peakValue(curBinCnt * binMax);
      double truncMean(peakValue);

      short binOffset(1);

      // This loop to develop the average
      // In theory, we could also keep the sum of the squares for the rms but I had problems doing
      // it that way so will loop twice... (potential time savings goes here!)
      while(curBinCnt < minNumBins)
      {
        if (binAdcMap[binMax-binOffset])
        {
          curBinCnt += binAdcMap[binMax-binOffset];
          truncMean += double(binAdcMap[binMax-binOffset] * (binMax - binOffset));
        }

        if (binAdcMap[binMax+binOffset])
        {
          curBinCnt += binAdcMap[binMax+binOffset];
          truncMean += double(binAdcMap[binMax+binOffset] * (binMax + binOffset));
        }

        binOffset++;
      }

      truncMean /= double(curBinCnt);

      binOffset  = 1;

      int    rmsBinCnt(binMaxCnt);
      double rmsVal(double(binMax)-truncMean);

      rmsVal *= double(rmsBinCnt) * rmsVal;

      // Second loop to get the rms
      while(rmsBinCnt < minNumBins)
      {
        if (binAdcMap[binMax-binOffset] > 0)
        {
          int    binIdx  = binMax - binOffset;
          int    binCnt  = binAdcMap[binIdx];
          double binVals = double(binIdx) - truncMean;

          rmsBinCnt += binCnt;
          rmsVal    += double(binCnt) * binVals * binVals;
        }

        if (binAdcMap[binMax+binOffset] > 0)
        {
          int    binIdx  = binMax + binOffset;
          int    binCnt  = binAdcMap[binIdx];
          double binVals = double(binIdx) - truncMean;

          rmsBinCnt += binCnt;
          rmsVal    += double(binCnt) * binVals * binVals;
        }

        binOffset++;
      }

      rmsVal = std::sqrt(std::max(0.,rmsVal / double(rmsBinCnt)));
      if (rmsVal >= fRmsCut[geo->View(channel)]) {
	fProvider.AddNoisyChannel(channel);
      }

    }//end loop over raw digits
  }//end FindNoisyChannels function 

}//end namespace lariov

DEFINE_ART_SERVICE_INTERFACE_IMPL(lariov::SIOVChannelFilterService, lariov::IChannelFilterService)

#endif