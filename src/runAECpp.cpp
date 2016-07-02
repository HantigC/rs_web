/*------------------------------------------------------------------------

 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.

 --------------------------------------------------------------------------

 Test driver that reads text files or XCASs or XMIs and calls the annotator

 -------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------- */
/*       Include dependencies                                              */
/* ----------------------------------------------------------------------- */

#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <sstream>

#include <uima/api.hpp>
#include "uima/internal_aggregate_engine.hpp"
#include "uima/annotator_mgr.hpp"

#include <rs/utils/output.h>
#include <rs/utils/time.h>
#include <rs/utils/exception.h>
#include <rs/utils/common.h>
#include <rs/io/visualizer.h>
#include <rs/scene_cas.h>

#include <designator_integration_msgs/Designator.h>
#include <designator_integration_msgs/DesignatorCommunication.h>
#include <iai_robosherlock_msgs/SetRSContext.h>

#include <rs/utils/RSPipelineManager.h>
#include <rs_kbreasoning/DesignatorWrapper.h>
#include <rs_kbreasoning/KRDefinitions.h>
#include <rs_kbreasoning/RSControledAnalysisEngine.h>

// designator_integration classes
#include <designators/Designator.h>


#include <ros/ros.h>
#include <ros/package.h>

#include <json_prolog/prolog.h>

#undef OUT_LEVEL
#define OUT_LEVEL OUT_LEVEL_DEBUG

#define SEARCHPATH "/descriptors/analysis_engines/"
#define QUERY "QUERY"

// This mutex will be locked, if:
//   1) The RSAnalysisEngineManager wants to execute pipelines
//   or
//   2) If a service call has come in and needs several process calls
//      on a RSAnalysisEngine to complete
std::mutex processing_mutex;

class RSAnalysisEngineManager
{
private:
  std::vector<RSControledAnalysisEngine> engines;

  const bool useVisualizer;
  const bool waitForServiceCall;
  rs::Visualizer visualizer;

  ros::NodeHandle nh_;
  ros::Publisher desig_pub;

public:
  RSAnalysisEngineManager(const bool useVisualizer, const std::string &savePath,
                          const bool &waitForServiceCall, ros::NodeHandle n) :
    useVisualizer(useVisualizer),
    waitForServiceCall(waitForServiceCall),
    visualizer(savePath), nh_(n)
  {
    // Create/link up to a UIMACPP resource manager instance (singleton)
    outInfo("Creating resource manager"); // TODO: DEBUG
    uima::ResourceManager &resourceManager = uima::ResourceManager::createInstance("RoboSherlock"); // TODO: change topic?

    switch(OUT_LEVEL)
    {
    case OUT_LEVEL_NOOUT:
    case OUT_LEVEL_ERROR:
      resourceManager.setLoggingLevel(uima::LogStream::EnError);
      break;
    case OUT_LEVEL_INFO:
      resourceManager.setLoggingLevel(uima::LogStream::EnWarning);
      break;
    case OUT_LEVEL_DEBUG:
      resourceManager.setLoggingLevel(uima::LogStream::EnMessage);
      break;
    }

    desig_pub = nh_.advertise<designator_integration_msgs::DesignatorResponse>(std::string("result_advertiser"), 5);

  }

  ~RSAnalysisEngineManager()
  {
    uima::ResourceManager::deleteInstance();
  }

  // Returns a string with the prolog query to execute, based on the informations in the designator
  std::string buildPrologQueryFromDesignator(designator_integration::Designator *desig,
      bool &success)
  {
    success = false;
    if(!desig)
    {
      return "NULL POINTER PASSED TO RSAnalysisEngineManager::buildPrologQueryFromDesignator";
    }

    std::string ret = "";
    if(desig->childForKey("OBJECT"))
    {
      // If the designator contains a "type" key, the highlevel is looking for a specific object of Type XY.
      // Use the corresponding Prolog Rule for object pipeline generation
      ret = "build_pipeline_for_object('";
      // Fetch the accepted predicates from the Designator
      ret += desig->childForKey("OBJECT")->stringValue();
      ret += "', A)";
    }
    else
    {
      ret = "build_single_pipeline_from_predicates([";
      std::vector<std::string> queriedKeys;

      // Fetch the keys from the Designator
      std::list<std::string> allKeys = desig->keys();

      //add the ones that are interpretable to the queriedKeys;
      for(const auto key:allKeys)
      {
        if(std::find(rs_kbreasoning::rsQueryTerms.begin(),rs_kbreasoning::rsQueryTerms.end(),boost::to_lower_copy(key))!= std::end(rs_kbreasoning::rsQueryTerms))
        {
          queriedKeys.push_back(boost::to_lower_copy(key));
        }
        else
        {
         outWarn(key<<" is not a valid query-language term");
        }
      }

      //leave this for now: TODO: need to handle also values
      if(desig->childForKey("DETECTION"))
      {
        designator_integration::KeyValuePair *kvp = desig->childForKey("DETECTION");
        if(kvp->stringValue() == "PANCAKE")
        {
          queriedKeys.push_back("pancakedetector");
        }
      }

      for(int i = 0; i < queriedKeys.size(); i++)
      {
        ret += queriedKeys.at(i);
        if(i < queriedKeys.size() - 1)
        {
          ret += ",";
        }
      }

      ret += "], A)";
    }
    success = true;
    return ret;
  }

  // Create a vector of Annotator Names from the result of the knowrob_rs library.
  // This vector can be used as input for RSAnalysisEngine::setNextPipelineOrder
  std::vector<std::string> createPipelineFromPrologResult(std::string result)
  {
    std::vector<std::string> new_pipeline;

    // Strip the braces from the result
    result.erase(result.end() - 1);
    result.erase(result.begin());

    std::stringstream resultstream(result);

    std::string token;
    while(std::getline(resultstream, token, ','))
    {
      // erase leading whitespaces
      token.erase(token.begin(), std::find_if(token.begin(), token.end(), std::bind1st(std::not_equal_to<char>(), ' ')));
      outInfo("Planned Annotator " << token);

      // From the extracted tokens, remove the prefix
      std::string prefix("http://knowrob.org/kb/rs_components.owl#");
      int prefix_length = prefix.length();

      // Erase by length, to avoid string comparison
      token.erase(0, prefix_length);
      // outInfo("Annotator name sanitized: " << token );

      new_pipeline.push_back(token);
    }
    return new_pipeline;
  }

  bool resetAECallback(iai_robosherlock_msgs::SetRSContext::Request &req,
                       iai_robosherlock_msgs::SetRSContext::Response &res)
  {
    std::string newContextName = req.newAe, contextAEPath;
    std::vector<std::string> files;

    if(rs::common::getAEPaths(newContextName, contextAEPath))
    {
      outInfo("Setting new context: " << newContextName);
      files.push_back(contextAEPath);
      this->init(files);
      return true;
    }
    else
    {
      outError("Contexts need to have a an AE defined");
      return false;
    }
  }

  bool designatorAllSolutionsCallback(designator_integration_msgs::DesignatorCommunication::Request &req,
                                      designator_integration_msgs::DesignatorCommunication::Response &res)
  {
    return designatorCallbackLogic(req, res, true);
  }

  bool designatorSingleSolutionCallback(designator_integration_msgs::DesignatorCommunication::Request &req,
                                        designator_integration_msgs::DesignatorCommunication::Response &res)
  {
    return designatorCallbackLogic(req, res, false);
  }

  // Should prolog execute all solutions?
  //   -> set allSolutions=true
  bool designatorCallbackLogic(designator_integration_msgs::DesignatorCommunication::Request &req,
                               designator_integration_msgs::DesignatorCommunication::Response &res, bool allSolutions)
  {
    designator_integration::Designator *desigRequest = new designator_integration::Designator(req.request.designator);

    RSQuery *query = new RSQuery();
    std::string superClass = "";
    if(desigRequest != NULL)
    {
      std::list<std::string> keys = desigRequest->keys();
      for(auto key: keys)
      {
        if(key == "TIMESTAMP")
        {
          designator_integration::KeyValuePair *kvp = desigRequest->childForKey("TIMESTAMP");
          std::string ts = kvp->stringValue();
          query->timestamp = std::stoll(ts);
          outInfo("received timestamp:" << query->timestamp);
        }
        if(key == "LOCATION")
        {
          designator_integration::KeyValuePair *kvp = desigRequest->childForKey("LOCATION");
          query->location = kvp->stringValue();
          outInfo("received location:" << query->location);
        }
        if(key == "OBJ-PARTS")
        {
          designator_integration::KeyValuePair *kvp = desigRequest->childForKey("OBJ-PARTS");
          query->objToInspect = kvp->stringValue();
          outInfo("received obj-part request for object: " << query->objToInspect);
        }
        if(key == "TYPE")
        {
          designator_integration::KeyValuePair *kvp = desigRequest->childForKey("TYPE");
          superClass = kvp->stringValue();
        }
      }
    }

    if(desigRequest->type() != designator_integration::Designator::OBJECT)
    {
      outInfo(" ***** RECEIVED SERVICE CALL WITH UNHANDELED DESIGNATOR TYPE (everything != OBJECT) ! Aborting... ****** ");
      return false;
    }

    if(rs::DesignatorWrapper::req_designator)
    {
      delete rs::DesignatorWrapper::req_designator;
    }

    rs::DesignatorWrapper::req_designator = new designator_integration::Designator(req.request.designator);
    outInfo("Received Designator call: ");
    rs::DesignatorWrapper::req_designator->printDesignator();

    std::string prologQuery = "";
    bool plGenerationSuccess = false;
    prologQuery = buildPrologQueryFromDesignator(desigRequest, plGenerationSuccess);

    if(!plGenerationSuccess)
    {
      outInfo("Aborting Prolog Query... The generated Prolog Command is invalid");
      return false;
    }
    outInfo("Query Prolog with the following command: " << prologQuery);
    json_prolog::Prolog pl;
    json_prolog::PrologQueryProxy bdgs = pl.query(prologQuery);


    if(bdgs.begin() == bdgs.end())
    {
      outInfo("Can't find solution for pipeline planning");
      return false; // Indicate failure
    }

    int pipelineId = 0;

    // Block the RSAnalysisEngineManager  - We need the engines now
    outInfo("acquiring lock");
    processing_mutex.lock();
    outInfo("lock acquired");
    designator_integration_msgs::DesignatorResponse designator_response;
    for(json_prolog::PrologQueryProxy::iterator it = bdgs.begin();
        it != bdgs.end(); it++)
    {
      json_prolog::PrologBindings bdg = *it;
      std::string prologResult = bdg["A"].toString();
      std::vector<std::string> new_pipeline_order = createPipelineFromPrologResult(bdg["A"].toString());

      //needed for saving results and returning them on a ros topic
      if(waitForServiceCall && !new_pipeline_order.empty())
      {
        new_pipeline_order.push_back("KBResultAdvertiser");
      }
      outInfo(FG_BLUE << "Executing Pipeline #" << pipelineId);

      // First version. Change the pipeline on the first engine
      // to a fixed set


      if(engines.size() > 0)
      {
        // This designator response will hold the OBJECT designators with the detected
        // annotations for every detected object

        outInfo("Executing pipeline generated by service call");
        engines.at(0).process(new_pipeline_order, true, designator_response, query);
        outInfo("Returned " << designator_response.designators.size() << " designators on this execution");

        // Add the PIPELINEID to reference the pipeline that was responsible for detecting
        // the returned object
        for(auto & designator : designator_response.designators)
        {
          // Convert the designator msg object to a normal Designator
          designator_integration::Designator d(designator);
          // Insert the current PIPELINEID
          d.setValue("PIPELINEID", pipelineId);
          designator = d.serializeToMessage();
        }

        // Define an ACTION designator with the planned pipeline
        designator_integration::Designator pipeline_action;
        pipeline_action.setType(designator_integration::Designator::ACTION);
        std::list<designator_integration::KeyValuePair *> lstDescription;
        for(auto & annotatorName : new_pipeline_order)
        {
          designator_integration::KeyValuePair *oneAnno = new designator_integration::KeyValuePair();
          oneAnno->setValue(annotatorName);
          lstDescription.push_back(oneAnno);
        }
        pipeline_action.setValue("PIPELINEID", pipelineId);
        pipeline_action.setValue("ANNOTATORS", designator_integration::KeyValuePair::LIST, lstDescription);
        designator_response.designators.push_back(pipeline_action.serializeToMessage());
        // Delete the allocated keyvalue pairs for the annotator names
        for(auto & kvpPtr : lstDescription)
        {
          delete kvpPtr;
        }
        outInfo("Executing pipeline generated by service call: done");
      }
      else
      {
        outInfo("ERROR: No engine set up");
        return false;
      }

      if(!allSolutions)
      {
        break;  // Only take the first solution if allSolutions == false
      }
      pipelineId++;
    }

    // All engine calls have been processed. Release the Lock
    processing_mutex.unlock();
    //now filter the shit out of the results:D

    std::vector<designator_integration::Designator> filteredResponse;
    std::vector<designator_integration::Designator> resultDesignators;

    for(auto & designator : designator_response.designators)
    {
      // Convert the designator msg object to a normal Designator
      designator_integration::Designator d(designator);
      resultDesignators.push_back(d);
      d.printDesignator();
      res.response.designators.push_back(d.serializeToMessage());
    }

    filterResults(*desigRequest, resultDesignators, filteredResponse, superClass);
    outInfo("The filteredResponse size:" << filteredResponse.size());

    designator_integration_msgs::DesignatorResponse topicResponse;
    for(auto & designator : filteredResponse)
    {
      overwriteParentField(designator, 0);
      topicResponse.designators.push_back(designator.serializeToMessage(false));
      res.response.designators.push_back(designator.serializeToMessage());
    }
    desig_pub.publish(topicResponse);

    delete query;
    outWarn("RS Query service call ended");
    return true;
  }

  //ugly ugly hack for openease
  void overwriteParentField(designator_integration::KeyValuePair &d, int level)
  {
    std::list<designator_integration::KeyValuePair *> children =  d.children();
    if(!children.empty())
    {
      for(std::list<designator_integration::KeyValuePair *>::iterator it = children.begin(); it != children.end(); ++it)
      {
        (*it)->setParent(level);
        overwriteParentField(**it, level + 1);
      }
    }
  }

  void filterResults(designator_integration::Designator &requestDesignator,
                     const std::vector<designator_integration::Designator> &resultDesignators,
                     std::vector<designator_integration::Designator> &filteredResponse,
                     std::string superclass)
  {
    outInfo("filtering the results based on the designator request");

    std::vector<bool> keep_designator;
    keep_designator.resize(resultDesignators.size(), true);

    //    requestDesignator.printDesignator();
    std::list<designator_integration::KeyValuePair *> requested_kvps = requestDesignator.description();

    for(std::list<designator_integration::KeyValuePair *>::iterator it = requested_kvps.begin(); it != requested_kvps.end(); ++it)
    {
      designator_integration::KeyValuePair req_kvp = **it;
      if(req_kvp.key() == "TIMESTAMP" || req_kvp.key() == "LOCATION")
      {
        continue;
      }
      if(req_kvp.key() == "HANDLE")
      {
        outInfo("Handle requested, nothing to do here");
        continue;
      }

      outInfo("No. of result Designators: " << resultDesignators.size());
      for(size_t i = 0; i < resultDesignators.size(); ++i)
      {
        designator_integration::Designator resDesig = resultDesignators[i];
        //        resDesig.printDesignator();
        //and here come the hacks
        std::vector<designator_integration::KeyValuePair *> resultsForRequestedKey;
        designator_integration::KeyValuePair *childRequestedKey = NULL;
        if(resDesig.childForKey("CLUSTERID") != NULL)
        {
          if(req_kvp.key() == "SIZE") //size is nested get it from the bounding box..bad design
          {
            childRequestedKey = resDesig.childForKey("BOUNDINGBOX")->childForKey("SIZE");
            resultsForRequestedKey.push_back(childRequestedKey);
          }
          else if(req_kvp.key() == "VOLUME")
          {
            childRequestedKey = resDesig.childForKey("VOLUME");
            if(childRequestedKey != NULL)
            {
              resultsForRequestedKey.push_back(childRequestedKey);
            }
            else
            {
              keep_designator[i] = false;
            }
          }

          else if(req_kvp.key() == "CONTAINS")
          {
            childRequestedKey = resDesig.childForKey("CONTAINS");
            if(childRequestedKey != NULL)
            {
              resultsForRequestedKey.push_back(childRequestedKey);
            }
            else
            {
              keep_designator[i] = false;
            }
          }
          else if(req_kvp.key() == "SHAPE") //there can be multiple shapes and these are not nested
          {
            std::list<designator_integration::KeyValuePair *> resKvPs = resDesig.description();
            for(std::list<designator_integration::KeyValuePair *>::iterator it2 = resKvPs.begin(); it2 != resKvPs.end(); ++it2)
            {
              designator_integration::KeyValuePair res_kvp = **it2;
              if(res_kvp.key() == "SHAPE")
              {
                resultsForRequestedKey.push_back(*it2);
              }
            }
          }
          else if(req_kvp.key() == "TYPE")//this shit needed so we don't loose al of our stuff just because all was sent instead of detection
          {
            resultsForRequestedKey.push_back(resDesig.childForKey("DETECTION"));
          }
          else
          {
            resultsForRequestedKey.push_back(resDesig.childForKey(req_kvp.key()));
          }
        }
        else
        {
          outWarn("No CLUSTER ID");
        }


        if(!resultsForRequestedKey.empty())
        {
          bool ok = false;
          for(int j = 0; j < resultsForRequestedKey.size(); ++j)
          {
            if(resultsForRequestedKey[j] != NULL)
            {
              //I am doing this wrong...why do I want to look at this shit again?
              //treat color differently because it is nested and has every color with ration in there
              if(resultsForRequestedKey[j]->key() == "COLOR")
              {
                std::list<designator_integration::KeyValuePair *> colorRatioPairs = resultsForRequestedKey[j]->children();
                for(auto iter = colorRatioPairs.begin(); iter != colorRatioPairs.end(); ++iter)
                {
                  designator_integration::KeyValuePair colorRatioKvp = **iter;
                  if(strcasecmp(colorRatioKvp.key().c_str(), req_kvp.stringValue().c_str()) == 0 || req_kvp.stringValue() == "")
                  {
                    outInfo("Color name mathces, ratio is: " << colorRatioKvp.floatValue());
                    if(colorRatioKvp.floatValue() > 0.20)
                    {
                      ok = true;
                    }
                  }
                }
              }
              if(resultsForRequestedKey[j]->key() == "VOLUME")
              {
                float volumeofCurrentObj = resultsForRequestedKey[j]->floatValue();
                outWarn("Volume as a float: " << volumeofCurrentObj);
                if(req_kvp.stringValue() == "")
                {
                  ok = true;
                }
                else
                {
                  float volumeAsked = atof(req_kvp.stringValue().c_str());
                  outWarn("Volume asked as float: " << volumeAsked);
                  if(volumeAsked <= volumeofCurrentObj)
                  {
                    ok = true;
                  }
                }
              }
              if(resultsForRequestedKey[j]->key() == "CONTAINS")
              {
                if(req_kvp.stringValue() == "")
                {
                  ok = true;
                }
                else
                {
                  std::string substanceName = resultsForRequestedKey[j]->childForKey("SUBSTANCE")->stringValue();
                  std::string substanceAsked = req_kvp.stringValue();
                  outWarn("Substance asked : " << substanceAsked);
                  if(strcasecmp(substanceName.c_str(), substanceAsked.c_str()) == 0)
                  {
                    ok = true;
                  }
                }
              }


              //another nested kv-p...we need a new interface...this one sux
              if(resultsForRequestedKey[j]->key() == "DETECTION")
              {
                std::list<designator_integration::KeyValuePair *> childrenPairs = resultsForRequestedKey[j]->children();
                for(auto iter = childrenPairs.begin(); iter != childrenPairs.end(); ++iter)
                {
                  designator_integration::KeyValuePair childrenPair = **iter;
                  if(childrenPair.key() == "TYPE")
                  {
                    if(superclass != "")
                    {
                      outWarn("filtering based on JSON required");
                      outWarn("Object looked at: " << childrenPair.stringValue());
                      std::stringstream prologQuery;
                      outWarn("Object should be subclass of: " << rs_kbreasoning::krNameMapping[superclass]);
                      prologQuery << "owl_subclass_of(" << rs_kbreasoning::krNameMapping[childrenPair.stringValue()] << "," << rs_kbreasoning::krNameMapping[superclass] << ").";


                      outWarn(prologQuery.str());
                      json_prolog::Prolog pl;
                      std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
                      json_prolog::PrologQueryProxy bdgs = pl.query(prologQuery.str());
                      std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
                      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
                      outInfo("Querying through json_prolog took: " << duration << " ms");
                      if(bdgs.begin() == bdgs.end())
                      {
                        outInfo(rs_kbreasoning::krNameMapping[childrenPair.stringValue()] << " IS NOT " << rs_kbreasoning::krNameMapping[superclass]);
                        ok = false;
                      }
                      else
                      {
                        ok = true;
                      }
                    }
                    else if(strcasecmp(childrenPair.stringValue().c_str(), req_kvp.stringValue().c_str()) == 0 || req_kvp.stringValue() == "")
                    {
                      ok = true;
                    }
                    else if(childrenPair.stringValue() == "bottle_acid" || childrenPair.stringValue() == "bottle_base")
                    {
                      std::string new_name = "bottle";
                      if(strcasecmp(new_name.c_str(), req_kvp.stringValue().c_str()) == 0)
                      {
                        ok = true;
                      }
                    }
                  }
                }
              }
              else if(strcasecmp(resultsForRequestedKey[j]->stringValue().c_str(),
                                 req_kvp.stringValue().c_str()) == 0 || req_kvp.stringValue() == "")
              {
                ok = true;
              }
            }
          }
          if(!ok)
          {
            keep_designator[i] = false;
          }
        }
      }
    }
    for(int i = 0; i < keep_designator.size(); ++i)
    {
      if(keep_designator[i])
      {
        outInfo("Designator: " << i << " is a match");
        filteredResponse.push_back(resultDesignators[i]);
      }
    }
  }

  void init(const std::vector<std::string> &files)
  {
    engines.resize(files.size());
    for(size_t i = 0; i < engines.size(); ++i)
    {
      engines[i].init(files[i]);
    }
    if(useVisualizer)
    {
      visualizer.start();
    }
  }

  void run()
  {
    for(; ros::ok();)
    {

      if(waitForServiceCall)
      {
        usleep(100000);
      }
      else
      {
        processing_mutex.lock();

        for(size_t i = 0; i < engines.size(); ++i)
        {
          engines[i].process(true);
        }
        processing_mutex.unlock();
      }

      ros::spinOnce();
    }
  }

  void stop()
  {
    if(useVisualizer)
    {
      visualizer.stop();
    }
    for(size_t i = 0; i < engines.size(); ++i)
    {
      engines[i].stop();
    }
  }
};

/* ----------------------------------------------------------------------- */
/*       Main                                                              */
/* ----------------------------------------------------------------------- */

/**
 * Error output if program is called with wrong parameter.
 */
void help()
{
  std::cout << "Usage: runAECpp [options] analysisEngine.xml [...]" << std::endl
            << "Options:" << std::endl
            << "  -wait If using piepline set this to wait for a service call" << std::endl
            << "  -visualizer  Enable visualization" << std::endl
            << "  -save PATH   Path for storing images" << std::endl;
}

int main(int argc, char *argv[])
{
  if(argc < 2)
  {
    help();
    return 1;
  }

  ros::init(argc, argv, std::string("RoboSherlock_") + getenv("USER"));

  std::vector<std::string> args;
  args.resize(argc - 1);
  for(int argI = 1; argI < argc; ++argI)
  {
    args[argI - 1] = argv[argI];
  }
  bool useVisualizer = false;
  bool waitForServiceCall = false;
  std::string savePath = getenv("HOME");

  size_t argO = 0;
  for(size_t argI = 0; argI < args.size(); ++argI)
  {
    const std::string &arg = args[argI];

    if(arg == "-visualizer")
    {
      useVisualizer = true;
    }
    else if(arg == "-wait")
    {
      waitForServiceCall = true;
    }
    else if(arg == "-save")
    {
      if(++argI < args.size())
      {
        savePath = args[argI];
      }
      else
      {
        outError("No save path defined!");
        return -1;
      }
    }
    else
    {
      args[argO] = args[argI];
      ++argO;
    }
  }
  args.resize(argO);

  struct stat fileStat;
  if(stat(savePath.c_str(), &fileStat) || !S_ISDIR(fileStat.st_mode))
  {
    outError("Save path \"" << savePath << "\" does not exist.");
    return -1;
  }

  std::vector<std::string> analysisEngineFiles;
  analysisEngineFiles.resize(args.size(), "");
  for(int argI = 0; argI < args.size(); ++argI)
  {
    const std::string &arg = args[argI];
    rs::common::getAEPaths(arg,analysisEngineFiles[argI]);
    if(analysisEngineFiles[argI].empty())
    {
      outError("analysis engine \"" << arg << "\" not found.");
      return -1;
    }
  }

  ros::NodeHandle n("~");
  try
  {
    RSAnalysisEngineManager manager(useVisualizer, savePath, waitForServiceCall, n);
    ros::ServiceServer service, singleService, setContextService;

    // Call this service, if RoboSherlock should try out every possible pipeline
    // that has been generated by the pipeline planning
    service = n.advertiseService("designator_request/all_solutions",
                                 &RSAnalysisEngineManager::designatorAllSolutionsCallback, &manager);

    // Call this service, if RoboSherlock should try out only
    // the pipeline with all Annotators, that provide the requested types (for example shape)
    singleService = n.advertiseService("designator_request/single_solution",
                                       &RSAnalysisEngineManager::designatorSingleSolutionCallback, &manager);

    // Call this service to switch between AEs
    setContextService = n.advertiseService("set_context",
                                           &RSAnalysisEngineManager::resetAECallback, &manager);


    manager.init(analysisEngineFiles);
    manager.run();
    manager.stop();
  }
  catch(const rs::Exception &e)
  {
    outError("Exception: " << std::endl << e.what());
    return -1;
  }
  catch(const uima::Exception &e)
  {
    outError("Exception: " << std::endl << e);
    return -1;
  }
  catch(const std::exception &e)
  {
    outError("Exception: " << std::endl << e.what());
    return -1;
  }
  catch(...)
  {
    outError("Unknown exception!");
    return -1;
  }
  return 0;
}
