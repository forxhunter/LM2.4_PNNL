/*
 * University of Illinois Open Source License
 * Copyright 2008-2018 Luthey-Schulten Group,
 * Copyright 2012 Roberts Group,
 * All rights reserved.
 * 
 * Developed by: Luthey-Schulten Group
 * 			     University of Illinois at Urbana-Champaign
 * 			     http://www.scs.uiuc.edu/~schulten
 * 
 * Developed by: Roberts Group
 * 			     Johns Hopkins University
 * 			     http://biophysics.jhu.edu/roberts/
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the Software), to deal with 
 * the Software without restriction, including without limitation the rights to 
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies 
 * of the Software, and to permit persons to whom the Software is furnished to 
 * do so, subject to the following conditions:
 * 
 * - Redistributions of source code must retain the above copyright notice, 
 * this list of conditions and the following disclaimers.
 * 
 * - Redistributions in binary form must reproduce the above copyright notice, 
 * this list of conditions and the following disclaimers in the documentation 
 * and/or other materials provided with the distribution.
 * 
 * - Neither the names of the Luthey-Schulten Group, University of Illinois at
 * Urbana-Champaign, the Roberts Group, Johns Hopkins University, nor the names
 * of its contributors may be used to endorse or promote products derived from
 * this Software without specific prior written permission.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL 
 * THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR 
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR 
 * OTHER DEALINGS WITH THE SOFTWARE.
 *
 * Author(s): Elijah Roberts
 */

#include <iostream>
#include <string>
#include <map>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "config.h"
#if defined(MACOSX)
#include <sys/time.h>
#endif
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <google/protobuf/stubs/common.h>
#include "core/Print.h"
#include "core/Exceptions.h"
#include "core/Types.h"
#include "core/Math.h"
#include "core/util.h"
#ifdef OPT_CUDA
#include "cuda/lm_cuda.h"
#endif
#include "io/lm_hdf5.h"
#include "io/SimulationFile.h"
#include "DiffusionModel.pb.h"
#include "ReactionModel.pb.h"
#include "io/SimulationParameters.h"
#include "core/CheckpointSignaler.h"
#include "core/DataOutputQueue.h"
#include "core/LocalDataOutputWorker.h"
#include "cmd/common.h"
#include "core/Globals.h"
#include "core/SignalHandler.h"
#include "core/ReplicateRunner.h"
#include "core/ResourceAllocator.h"
#include "SimulationParameters.pb.h"
#include "thread/Thread.h"
#include "thread/WorkerManager.h"
#include "lptf/Profile.h"

using std::map;
using std::list;
using lm::Print;
using lm::Exception;
using lm::main::ReplicateRunner;
using lm::main::ResourceAllocator;
using lm::me::MESolverFactory;
using lm::thread::PthreadException;

void listDevices();
void executeSimulation();
ReplicateRunner * startReplicate(int replicate, MESolverFactory solverFactory, std::map<std::string,string> & simulationParameters, lm::io::ReactionModel * reactionModel, lm::io::DiffusionModel * diffusionModel, uint8_t * lattice, size_t latticeSize, uint8_t * latticeSites, size_t latticeSitesSize, ResourceAllocator & resourceAllocator);
ReplicateRunner * popNextFinishedReplicate(list<ReplicateRunner *> & runningReplicates, ResourceAllocator & resourceAllocator);

// Allocate the profile space.
PROF_ALLOC;

int main(int argc, char** argv)
{	
    // Make sure we are using the correct protocol buffers library.
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    PROF_INIT;
	try
	{
		printCopyright(argc, argv);
		parseArguments(argc, argv, "lm::rdme::MpdRdmeSolver");
		
		if (functionOption == "help")
		{
			printUsage(argc, argv);
		}
		else if (functionOption == "version")
		{
            printBuildConfig();
		}
		else if (functionOption == "devices")
		{
		    listDevices();
		}
        else if (functionOption == "simulation")
        {
            executeSimulation();
        }
		else
		{
			throw lm::CommandLineArgumentException("unknown function.");
		}
        Print::printf(Print::INFO, "Program execution finished.");
	    PROF_WRITE;
	    google::protobuf::ShutdownProtobufLibrary();
		return 0;
	}
    catch (lm::CommandLineArgumentException & e)
    {
    	std::cerr << "Invalid command line argument: " << e.what() << std::endl << std::endl;
        printUsage(argc, argv);
    }
    catch (lm::Exception & e)
    {
    	std::cerr << "Exception during execution: " << e.what() << std::endl;
    }
    catch (std::exception & e)
    {
    	std::cerr << "Exception during execution: " << e.what() << std::endl;
    }
    catch (...)
    {
    	std::cerr << "Unknown Exception during execution." << std::endl;
    }
    PROF_WRITE;
    google::protobuf::ShutdownProtobufLibrary();
    return -1;
}

void listDevices()
{
    printf("Running with %d/%d processor(s)", numberCpuCores, getPhysicalCpuCores());

    #ifdef OPT_CUDA
    printf(" and %d/%d CUDA device(s)", (int)cudaDevices.size(), lm::CUDA::getNumberDevices());
    #endif
    printf(".\n");

    #ifdef OPT_CUDA
    if (shouldPrintCudaCapabilities)
    {
        for (int i=0; i<(int)cudaDevices.size(); i++)
        {
            printf("  %s\n", lm::CUDA::getCapabilitiesString(cudaDevices[i]).c_str());
        }
    }
    #endif
}

void executeSimulation()
{
    PROF_SET_THREAD(0);
    PROF_BEGIN(PROF_SIM_RUN);

    Print::printf(Print::DEBUG, "Master process started.");

    // Create the resource allocator, subtract one core for the data output thread.
    #ifdef OPT_CUDA
    Print::printf(Print::INFO, "Using %d processor(s) and %d CUDA device(s) per process.", numberCpuCores, (int)cudaDevices.size());
    Print::printf(Print::INFO, "Assigning %0.2f processor(s) and %0.2f CUDA device(s) per replicate.", cpuCoresPerReplicate, cudaDevicesPerReplicate);
    ResourceAllocator resourceAllocator(0, numberCpuCores, cpuCoresPerReplicate, cudaDevices, cudaDevicesPerReplicate);
    #else
    Print::printf(Print::INFO, "Using %d processor(s) per process.", numberCpuCores);
    Print::printf(Print::INFO, "Assigning %0.2f processor(s) per replicate.", cpuCoresPerReplicate);
    ResourceAllocator resourceAllocator(0, numberCpuCores, cpuCoresPerReplicate);
    #endif

    // Reserve a core for the data output thread, unless we have a flag telling us not to.
    int reservedCpuCore = 0;
    if (shouldReserveOutputCore)
    {
        reservedCpuCore=resourceAllocator.reserveCpuCore();
        Print::printf(Print::INFO, "Reserved CPU core %d for data output.", reservedCpuCore);
    }

    // Create a worker to handle any signals.
    lm::main::SignalHandler * signalHandler = new lm::main::SignalHandler();
    signalHandler->setAffinity(reservedCpuCore);
    signalHandler->start();

    // Create the checkpoint signaler.
    lm::main::CheckpointSignaler * checkpointSignaler = new lm::main::CheckpointSignaler();
    checkpointSignaler->setAffinity(reservedCpuCore);
    checkpointSignaler->start();
    checkpointSignaler->startCheckpointing(checkpointInterval);

    // Open the file.
    lm::io::hdf5::SimulationFile * file = new lm::io::hdf5::SimulationFile(simulationFilename);

    // Start the data output thread.
    lm::main::LocalDataOutputWorker * dataOutputWorker = new lm::main::LocalDataOutputWorker(file);
    dataOutputWorker->setAffinity(reservedCpuCore);
    dataOutputWorker->start();

    // Set the data output handler to be the worker.
    lm::main::DataOutputQueue::setInstance(dataOutputWorker);

    // Create a table for tracking the replicate assignments.
    int assignedSimulations=0;

    // Get the maximum number of simulations that can be started on each process.
    int maxSimulations = resourceAllocator.getMaxSimultaneousReplicates();
    Print::printf(Print::INFO, "Number of simultaneous replicates is %d", maxSimulations);
    if (maxSimulations == 0) throw Exception("Invalid configuration, no replicates can be processed.");

    // Create a table for the simulation status.
    map<int,int> simulationStatusTable;
    map<int,struct timespec> simulationStartTimeTable;
    for (vector<int>::iterator it=replicates.begin(); it<replicates.end(); it++)
    {
        simulationStatusTable[*it] = 0;
        simulationStartTimeTable[*it].tv_sec = 0;
        simulationStartTimeTable[*it].tv_nsec = 0;
    }

    // Get the simulation parameters.
    std::map<std::string,string> simulationParameters = file->getParameters();

	// Inject any command-line parameters
    for (vector<string>::iterator it=cmdline_parameters.begin(); it<cmdline_parameters.end(); it++)
	{
		size_t eqpos = (*it).find("=");
		if(eqpos == string::npos)
		{
			Print::printf(Print::WARNING, "Malformed parameter setting \"%s\"", (*it).c_str());
		}
		else
		{
			string k = (*it).substr(0, eqpos);
			string v = (*it).substr(eqpos+1);
			Print::printf(Print::INFO, "Setting simulation parameter \"%s\" = \"%s\"", k.c_str(), v.c_str());
			simulationParameters[k]=v;
		}
	}

    // Get the reaction model.
    lm::io::ReactionModel reactionModel;
    if (solverFactory.needsReactionModel())
    {
        file->getReactionModel(&reactionModel);
    }

    // Get the diffusion model.
    lm::io::DiffusionModel diffusionModel;
    uint8_t * lattice=NULL, * latticeSites=NULL;
    size_t latticeSize=0, latticeSitesSize=0;
    if (solverFactory.needsDiffusionModel())
    {
        file->getDiffusionModel(&diffusionModel);

		// allocate x*y*z*p*sizeof(p) memory for lattice.  I guess it is going to have to be up
		// to the solver to decide what to do with it based on bytes_per_particle.
        latticeSize = size_t(diffusionModel.lattice_x_size())*size_t(diffusionModel.lattice_y_size())*size_t(diffusionModel.lattice_z_size())*size_t(diffusionModel.particles_per_site())*size_t(diffusionModel.bytes_per_particle());

        lattice = new uint8_t[latticeSize];
        latticeSitesSize = diffusionModel.lattice_x_size()*diffusionModel.lattice_y_size()*diffusionModel.lattice_z_size();
        latticeSites = new uint8_t[latticeSitesSize];
        file->getDiffusionModelLattice(&diffusionModel, lattice, latticeSize, latticeSites, latticeSitesSize);

/*		"upscale" the 8-bit particle to a 32-bit particle lattice
		if(diffusionModel.bytes_per_particle() != 4)
		{
			uint32_t *biggerLattice = new uint32_t[latticeSize];
			for(int i=0; i<latticeSize; i++)
			{
				biggerLattice[i]=lattice[i];
			}
			delete[] lattice;
			lattice = (uint8_t*)biggerLattice;
			diffusionModel.set_bytes_per_particle(4);
			latticeSize *= 4;
		}
*/
    }

    // Distribute the simulations to the processes.
    Print::printf(Print::INFO, "Starting %d replicates from file %s.", replicates.size(), simulationFilename.c_str());
    list<ReplicateRunner *> runningReplicates;
    unsigned long long noopLoopCycles=0;
    while (!globalAbort)
    {
        // Increment the loop counter.
        noopLoopCycles++;

        // Check for finished simulations in our process.
        ReplicateRunner * finishedReplicate;
        while ((finishedReplicate=popNextFinishedReplicate(runningReplicates, resourceAllocator)) != NULL)
        {
            PROF_BEGIN(PROF_MASTER_FINISHED_THREAD);

            struct timespec now;
            #if defined(LINUX)
            clock_gettime(CLOCK_REALTIME, &now);
            #elif defined(MACOSX)
            struct timeval now2;
            gettimeofday(&now2, NULL);
            now.tv_sec = now2.tv_sec;
            now.tv_nsec = now2.tv_usec*1000;
            #endif

            Print::printf(Print::INFO, "Replicate %d completed with exit code %d in %0.2f seconds.", finishedReplicate->getReplicate(), finishedReplicate->getReplicateExitCode(), ((double)(now.tv_sec-simulationStartTimeTable[finishedReplicate->getReplicate()].tv_sec))+1e-9*((double)now.tv_nsec-simulationStartTimeTable[finishedReplicate->getReplicate()].tv_nsec));
            assignedSimulations--;
            simulationStatusTable[finishedReplicate->getReplicate()] = 2;
            noopLoopCycles = 0;
            finishedReplicate->stop();
            delete finishedReplicate; // We are responsible for deleting the replicate runner.
            PROF_END(PROF_MASTER_FINISHED_THREAD);
        }

        // See if we need to start any new simulations and then wait a while.
        if (noopLoopCycles > 1000)
        {
            // Find a simulation to perform.
            int replicate = -1;
            bool allFinished=true;
            for (vector<int>::iterator it=replicates.begin(); it<replicates.end(); it++)
            {
                if (simulationStatusTable[*it] == 0)
                {
                    replicate = *it;
                    allFinished = false;
                    break;
                }
                if (simulationStatusTable[*it] != 2) allFinished = false;
            }

            // If all of the simulations are finished, we are done.
            if (allFinished) break;

            // Find a process to perform the simulation.
            if (replicate >= 0 && assignedSimulations < maxSimulations)
            {
				// Otherwise it must be us, so start the replicate.
				runningReplicates.push_back(startReplicate(replicate, solverFactory, simulationParameters, &reactionModel, &diffusionModel, lattice, latticeSize, latticeSites, latticeSitesSize, resourceAllocator));

				assignedSimulations++;
				simulationStatusTable[replicate] = 1;
                struct timespec now;
                #if defined(LINUX)
                clock_gettime(CLOCK_REALTIME, &now);
                #elif defined(MACOSX)
                struct timeval now2;
                gettimeofday(&now2, NULL);
                now.tv_sec = now2.tv_sec;
                now.tv_nsec = now2.tv_usec*1000;
                #endif
                simulationStartTimeTable[replicate].tv_sec = now.tv_sec;
                simulationStartTimeTable[replicate].tv_nsec = now.tv_nsec;
				continue;
            }

            PROF_BEGIN(PROF_MASTER_SLEEP);
            unsigned int sleepTime = 1000000;
            if (noopLoopCycles > 2000) sleepTime = 10000000;
            if (noopLoopCycles > 2100) sleepTime = 100000000;
            if (noopLoopCycles >= 3000 && noopLoopCycles%1000 == 0)
            {
                int replicatesRunning=0, replicatesRemaining=0;
                for (vector<int>::iterator it=replicates.begin(); it<replicates.end(); it++)
                {
                    if (simulationStatusTable[*it] == 0)
                        replicatesRemaining++;
                    else if (simulationStatusTable[*it] == 1)
                        replicatesRunning++;
                }
                Print::printf(Print::INFO, "Master sleeping, waiting for %d replicates to finish, %d left to start.",replicatesRunning,replicatesRemaining);
            }
            struct timespec requested, remainder;
            requested.tv_sec  = 0;
            requested.tv_nsec = sleepTime;
            if (nanosleep(&requested, &remainder) != 0 && errno != EINTR) throw lm::Exception("Sleep failed.");
            PROF_END(PROF_MASTER_SLEEP);
        }
    }

    Print::printf(Print::INFO, "Master shutting down.");

    // Stop checkpointing.
    checkpointSignaler->stopCheckpointing();


    // If this was a global abort, stop the workers quickly.
    if (globalAbort)
    {
        Print::printf(Print::WARNING, "Aborting worker threads.");
        lm::thread::WorkerManager::getInstance()->abortWorkers();
    }

    // Otherwise, let them finish at their own pace.
    else
    {
        Print::printf(Print::DEBUG, "Stopping worker threads.");
        lm::thread::WorkerManager::getInstance()->stopWorkers();
    }

    // Close the simulation file.
    delete file;
    Print::printf(Print::INFO, "Simulation file closed.");

    // Cleanup any resources.
    delete checkpointSignaler;
    delete signalHandler;
    delete dataOutputWorker;
    if (lattice != NULL) delete [] lattice; lattice = NULL;
    if (latticeSites != NULL) delete [] latticeSites; latticeSites = NULL;

    Print::printf(Print::DEBUG, "Master process finished.");


    PROF_END(PROF_SIM_RUN);
}

ReplicateRunner * startReplicate(int replicate, MESolverFactory solverFactory, std::map<std::string,string> & simulationParameters, lm::io::ReactionModel * reactionModel, lm::io::DiffusionModel * diffusionModel, uint8_t * lattice, size_t latticeSize, uint8_t * latticeSites, size_t latticeSitesSize, ResourceAllocator & resourceAllocator)
{
    // Allocate resources for the replicate.
    ResourceAllocator::ComputeResources resources = resourceAllocator.assignReplicate(replicate);

    // Start a new thread for the replicate.
    Print::printf(Print::DEBUG, "Starting replicate %d (%s).", replicate, resources.toString().c_str());
    ReplicateRunner * runner = new ReplicateRunner(replicate, solverFactory, &simulationParameters, reactionModel, diffusionModel, lattice, latticeSize, latticeSites, latticeSitesSize, resources);
    runner->start();
    return runner;
}

ReplicateRunner * popNextFinishedReplicate(list<ReplicateRunner *> & runningReplicates, ResourceAllocator & resourceAllocator)
{
    for (list<ReplicateRunner *>::iterator it=runningReplicates.begin(); it != runningReplicates.end(); it++)
    {
        ReplicateRunner * runner = *it;
        if (runner->hasReplicateFinished())
        {
            runningReplicates.erase(it);
            resourceAllocator.removeReplicate(runner->getReplicate());
            return runner;
        }
    }
    return NULL;
}
