/*
 * Copyright 2011,
 *
 * Andrei Herdt
 * Francois Keith
 * Olivier Stasse
 *
 * JRL, CNRS/AIST
 *
 * This file is part of walkGenJrl.
 * walkGenJrl is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * walkGenJrl is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Lesser Public License for more details.
 * You should have received a copy of the GNU Lesser General Public License
 * along with walkGenJrl.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Research carried out within the scope of the
 *  Joint Japanese-French Robotics Laboratory (JRL)
 */

/*! This object composes all the objects necessary for the generation of walking
 * trajectories as proposed by Herdt Advanced Robotics 2010.

  Andrei Herdt,
  Olivier Stasse,
*/

#include "portability/gettimeofday.hh"

#ifdef WIN32
# include <Windows.h>
#endif /* WIN32 */

#include <time.h>

#include <iostream>
#include <fstream>

#include <ZMPRefTrajectoryGeneration/ZMPVelocityReferencedQP.h>

using namespace std;
using namespace PatternGeneratorJRL;



ZMPVelocityReferencedQP::ZMPVelocityReferencedQP(SimplePluginManager *lSPM,
						 string DataFile,
						 CjrlHumanoidDynamicRobot *aHS) :
  ZMPRefTrajectoryGeneration(lSPM)
{

  TimeBuffer_ = 0.040;
  QP_T_ = 0.1;
  QP_N_ = 16;
  m_SamplingPeriod = 0.005;
  PerturbationOccured_ = false;
  UpperTimeLimitToUpdate_ = 0.0;
  RobotMass_ = aHS->mass();
  //Feet distance in the DS phase


  // For computing the equilibrium constraints from the feet positions.
  RFC_ = new RelativeFeetInequalities(lSPM,aHS);

  OFTG_ = new OnLineFootTrajectoryGeneration(lSPM,aHS->leftFoot());
  OFTG_->InitializeInternalDataStructures();
  OFTG_->SetSingleSupportTime(0.7);
  OFTG_->SetDoubleSupportTime(QP_T_);
  OFTG_->QPSamplingPeriod(QP_T_);
  OFTG_->FeetDistance(0.2);

  SupportFSM_ = new SupportFSM();
  SupportFSM_->StepPeriod(0.8);
  SupportFSM_->DSPeriod(1e9);
  SupportFSM_->DSSSPeriod(0.8);
  SupportFSM_->NbStepsSSDS(200);
  SupportFSM_->SamplingPeriod(QP_T_);

  /* Orientations preview algorithm*/
  OrientPrw_ = new OrientationsPreview(aHS->rootJoint());
  OrientPrw_->SamplingPeriod(QP_T_);
  OrientPrw_->NbSamplingsPreviewed(QP_N_);
  OrientPrw_->SSLength(SupportFSM_->StepPeriod());
  COMState CurrentTrunkState;
  OrientPrw_->CurrentTrunkState(CurrentTrunkState);

  /// Initialize  the 2D LIPM
  CoM_.SetSimulationControlPeriod(QP_T_);
  CoM_.SetRobotControlPeriod(0.005);
  CoM_.SetComHeight(0.814);
  CoM_.InitializeSystem();

  VRQPGenerator_ = new GeneratorVelRef(lSPM );
  VRQPGenerator_->NbPrwSamplings(QP_N_);
  VRQPGenerator_->SamplingPeriodPreview(QP_T_);
  VRQPGenerator_->ComHeight(0.814);
  VRQPGenerator_->initialize_matrices();
  VRQPGenerator_->Ponderation( 1.0, IntermedQPMat::INSTANT_VELOCITY );
  VRQPGenerator_->Ponderation( 0.000001, IntermedQPMat::COP_CENTERING );
  VRQPGenerator_->Ponderation( 0.00001, IntermedQPMat::JERK_MIN );

  Door_.initialize(-M_PI/2.00);

  // Register method to handle
  string aMethodName[2] =
    {":previewcontroltime",
     ":numberstepsbeforestop"};

  for(int i=0;i<2;i++)
    {
      if (!RegisterMethod(aMethodName[i]))
        {
          std::cerr << "Unable to register " << aMethodName << std::endl;
        }
    }

}

ZMPVelocityReferencedQP::~ZMPVelocityReferencedQP()
{

  if (VRQPGenerator_!=0)
    delete VRQPGenerator_;

  if (SupportFSM_!=0)
    delete SupportFSM_;

  if (RFC_!=0)
    delete RFC_;

  if (OFTG_!=0)
    delete OFTG_;

  if (OrientPrw_!=0)
    delete OrientPrw_;

}


void ZMPVelocityReferencedQP::setVelReference(istringstream &strm)
{
  strm >> VelRef_.local.x;
  strm >> VelRef_.local.y;
  strm >> VelRef_.local.yaw;
}

void ZMPVelocityReferencedQP::setVelReference(double dx,
					      double dy,
					      double dyaw)
{
  VelRef_.local.x = dx;
  VelRef_.local.y = dy;
  VelRef_.local.yaw = dyaw;
}

void ZMPVelocityReferencedQP::setCoMPerturbationForce(istringstream &strm)
{
  MAL_VECTOR_RESIZE(PerturbationAcceleration_,6);

  strm >> PerturbationAcceleration_(2);
  strm >> PerturbationAcceleration_(5);
  PerturbationAcceleration_(2) = PerturbationAcceleration_(2)/RobotMass_;
  PerturbationAcceleration_(5) = PerturbationAcceleration_(5)/RobotMass_;
  PerturbationOccured_ = true;
}

void ZMPVelocityReferencedQP::setCoMPerturbationForce(double x,double y)
{
  MAL_VECTOR_RESIZE(PerturbationAcceleration_,6);

  PerturbationAcceleration_(2) = x/RobotMass_;
  PerturbationAcceleration_(5) = y/RobotMass_;
  PerturbationOccured_ = true;

}


void
ZMPVelocityReferencedQP::CallMethod(std::string & Method, std::istringstream &strm)
{
  if (Method==":previewcontroltime")
    {
      strm >> m_PreviewControlTime;
    }
  if (Method==":numberstepsbeforestop")
    {
      unsigned NbStepsSSDS;
      strm >> NbStepsSSDS;
      SupportFSM_->NbStepsSSDS(NbStepsSSDS);
    }

  ZMPRefTrajectoryGeneration::CallMethod(Method,strm);
}


int
ZMPVelocityReferencedQP::InitOnLine(deque<ZMPPosition> & FinalZMPTraj_deq,
					deque<COMState> & FinalCoMPositions,
					deque<FootAbsolutePosition> & FinalLeftFootTraj_deq,
					deque<FootAbsolutePosition> & FinalRightFootTraj_deq,
					FootAbsolutePosition & InitLeftFootAbsolutePosition,
					FootAbsolutePosition & InitRightFootAbsolutePosition,
					deque<RelativeFootPosition> &, // RelativeFootPositions,
					COMState & lStartingCOMState,
					MAL_S3_VECTOR_TYPE(double) & lStartingZMPPosition)
{

  FootAbsolutePosition CurrentLeftFootAbsPos, CurrentRightFootAbsPos;

  // Set the internal state of the ZMPRefTrajectory object.
  m_OnLineMode = true;
  EndingPhase_ = false;
  TimeToStopOnLineMode_ = -1.0;

  // Initialize position of the feet.
  CurrentLeftFootAbsPos = InitLeftFootAbsolutePosition;
  CurrentLeftFootAbsPos.z = 0.0;//OFTG_->m_AnklePositionLeft[2];
  CurrentLeftFootAbsPos.time = 0.0;
  CurrentLeftFootAbsPos.theta = 0.0;


  CurrentRightFootAbsPos = InitRightFootAbsolutePosition;
  CurrentRightFootAbsPos.z = 0.0;//OFTG_->m_AnklePositionRight[2];
  CurrentRightFootAbsPos.time = 0.0;
  CurrentRightFootAbsPos.theta = 0.0;

  // V pre is the difference between
  // the current SupportFSM_ position and the precedent.


  int AddArraySize;
  {
    assert(m_SamplingPeriod > 0);
    double ldAddArraySize = TimeBuffer_/m_SamplingPeriod;
    AddArraySize = (int) ldAddArraySize;
  }

  FinalZMPTraj_deq.resize(AddArraySize);
  FinalCoMPositions.resize(AddArraySize);
  FinalLeftFootTraj_deq.resize(AddArraySize);
  FinalRightFootTraj_deq.resize(AddArraySize);
  int CurrentZMPindex=0;
  for( unsigned i=0;i<FinalZMPTraj_deq.size();i++)
    {

      // Smooth ramp
      FinalZMPTraj_deq[CurrentZMPindex].px = lStartingZMPPosition(0);
      FinalZMPTraj_deq[CurrentZMPindex].py = lStartingZMPPosition(1);
      FinalZMPTraj_deq[CurrentZMPindex].pz = lStartingZMPPosition(2);
      FinalZMPTraj_deq[CurrentZMPindex].theta = 0.0;
      FinalZMPTraj_deq[CurrentZMPindex].time = m_CurrentTime;
      FinalZMPTraj_deq[CurrentZMPindex].stepType = 0;

      // Set CoM positions.
      FinalCoMPositions[CurrentZMPindex] = lStartingCOMState;
      // Set Left Foot positions.
      FinalLeftFootTraj_deq[CurrentZMPindex] = CurrentLeftFootAbsPos;
      FinalRightFootTraj_deq[CurrentZMPindex] = CurrentRightFootAbsPos;

      FinalLeftFootTraj_deq[CurrentZMPindex].time =
        FinalRightFootTraj_deq[CurrentZMPindex].time = m_CurrentTime;

      FinalLeftFootTraj_deq[CurrentZMPindex].stepType =
        FinalRightFootTraj_deq[CurrentZMPindex].stepType = 10;

      m_CurrentTime += m_SamplingPeriod;
      CurrentZMPindex++;

    }

  // INITIALIZE CENTER OF MASS:
  // --------------------------
  com_t CoM;
  CoM.x[0] = lStartingCOMState.x[0];
  CoM.x[1] = lStartingCOMState.x[1];
  CoM.x[2] = lStartingCOMState.x[2];
  CoM.y[0] = lStartingCOMState.y[0];
  CoM.y[1] = lStartingCOMState.y[1];
  CoM.y[2] = lStartingCOMState.y[2];
  CoM_(CoM);

  return 0;
}


void
ZMPVelocityReferencedQP::OnLine(double time,
				     deque<ZMPPosition> & FinalZMPTraj_deq,
				     deque<COMState> & FinalCOMTraj_deq,
				     deque<FootAbsolutePosition> &FinalLeftFootTraj_deq,
				     deque<FootAbsolutePosition> &FinalRightFootTraj_deq)
{

  // If on-line mode not activated we go out.
  if (!m_OnLineMode)
    { return; }

  // Testing if we are reaching the end of the online mode.
  if ((EndingPhase_) &&
      (time>=TimeToStopOnLineMode_))
    { m_OnLineMode = false; }


  // Apply external forces if occured
  if(PerturbationOccured_ == true)
    {
      com_t com = CoM_();
      com.x(2) = com.x(2)+PerturbationAcceleration_(2);
      com.y(2) = com.y(2)+PerturbationAcceleration_(5);
      PerturbationOccured_ = false;
      CoM_(com);
    }

  // UPDATE WALKING TRAJECTORIES:
  // ----------------------------
  if(time + 0.00001 > UpperTimeLimitToUpdate_)
    {
      double TotalAmountOfCPUTime=0.0,CurrentCPUTime=0.0;
      struct timeval start,end;
      gettimeofday(&start,0);


      // UPDATE INTERNAL DATA:
      // ---------------------
      VRQPGenerator_->Reference(VelRef_);
      VRQPGenerator_->CurrentTime(time+TimeBuffer_);
      VRQPGenerator_->CoM(CoM_());


      // PREVIEW SUPPORT STATES FOR THE WHOLE PREVIEW WINDOW:
      // ----------------------------------------------------
      deque<support_state_t> PrwSupportStates_deq;
      VRQPGenerator_->preview_support_states(SupportFSM_, PrwSupportStates_deq);


      // DETERMINE CURRENT SUPPORT POSITION:
      // -----------------------------------
      support_state_t CurrentSupport = PrwSupportStates_deq.front();
      //Add a new support foot to the support feet history deque
      if(CurrentSupport.StateChanged == true)
        {
          FootAbsolutePosition FAP;
          if(CurrentSupport.Foot==1)
            FAP = FinalLeftFootTraj_deq.back();
          else
            FAP = FinalRightFootTraj_deq.back();
          CurrentSupport.x = FAP.x;
          CurrentSupport.y = FAP.y;
          CurrentSupport.yaw = FAP.theta*M_PI/180.0;
          CurrentSupport.StartTime = m_CurrentTime;
          VRQPGenerator_->SupportState( CurrentSupport );
        }


      // COMPUTE ORIENTATIONS OF FEET FOR WHOLE PREVIEW PERIOD:
      // ------------------------------------------------------
      deque<double> PreviewedSupportAngles_deq;
      OrientPrw_->preview_orientations(time+TimeBuffer_,
                                VelRef_,
				SupportFSM_->StepPeriod(), CurrentSupport,
				FinalLeftFootTraj_deq, FinalRightFootTraj_deq,
	                        PreviewedSupportAngles_deq);


      // COMPUTE REFERENCE IN THE GLOBAL FRAME:
      // --------------------------------------
      VRQPGenerator_->compute_global_reference( FinalCOMTraj_deq );


      // BUILD CONSTANT PART OF THE OBJECTIVE:
      // -------------------------------------
      VRQPGenerator_->build_invariant_part( Problem_ );


      // BUILD VARIANT PART OF THE OBJECTIVE:
      // ------------------------------------
      VRQPGenerator_->update_problem( Problem_, PrwSupportStates_deq );


      // BUILD CONSTRAINTS:
      // ------------------
      VRQPGenerator_->build_constraints( Problem_,
          RFC_,
          FinalLeftFootTraj_deq,
          FinalRightFootTraj_deq,
          PrwSupportStates_deq,
          PreviewedSupportAngles_deq );


      // BUILD DOOR CONSTRAINTS:
      // -----------------------
      double DesVelDoor = 0.0;
      VRQPGenerator_->build_constraints_door( time, DesVelDoor,
		      Door_, PrwSupportStates_deq, Problem_);


      // SOLVE PROBLEM:
      // --------------
      QPProblem_s::solution_t Result;
      Problem_.solve( QPProblem_s::QLD , Result );


      char FileName[1024];
      sprintf(FileName,"/tmp/Problem%f.dat", time);
      Problem_.dump_problem(FileName);
      cout<<"time:"<<time<<endl;

      Problem_.reset();

      for(int i=0;i<PrwSupportStates_deq.back().StepNumber;i++)
      {
          cout<<"X"<<i<<":"<<Result.Solution_vec(2*QP_N_+i)<<endl;
          cout<<"Y"<<i<<":"<<Result.Solution_vec(2*QP_N_+PrwSupportStates_deq.back().StepNumber+i)<<endl;
          cout<<"LMult."<<Result.ConstrLagr_vec<<endl;
      }


      // INTERPOLATE THE NEXT COMPUTED COM STATE:
      // ----------------------------------------
      FinalCOMTraj_deq.resize((int)((QP_T_+TimeBuffer_)/m_SamplingPeriod));
      FinalZMPTraj_deq.resize((int)((QP_T_+TimeBuffer_)/m_SamplingPeriod));
      FinalLeftFootTraj_deq.resize((int)((QP_T_+TimeBuffer_)/m_SamplingPeriod));
      FinalRightFootTraj_deq.resize((int)((QP_T_+TimeBuffer_)/m_SamplingPeriod));
      int CurrentIndex = (int)(TimeBuffer_/m_SamplingPeriod)-1;
      CoM_.Interpolation(FinalCOMTraj_deq,
			      FinalZMPTraj_deq,
			      CurrentIndex,
			      Result.Solution_vec[0],Result.Solution_vec[QP_N_]);
      CoM_.OneIteration(Result.Solution_vec[0],Result.Solution_vec[QP_N_]);


      // COMPUTE ORIENTATION OF TRUNK:
      // -----------------------------
      OrientPrw_->interpolate_trunk_orientation(time+TimeBuffer_, CurrentIndex,
                            m_SamplingPeriod, CurrentSupport,
                            FinalCOMTraj_deq);


      // INTERPOLATE THE COMPUTED FEET POSITIONS:
      // ----------------------------------------
      unsigned NumberStepsPrwd = PrwSupportStates_deq.back().StepNumber;
      OFTG_->interpolate_feet_positions(time+TimeBuffer_,
                               CurrentIndex, CurrentSupport,
                               Result.Solution_vec[2*QP_N_], Result.Solution_vec[2*QP_N_+NumberStepsPrwd],
                               PreviewedSupportAngles_deq,
			       FinalLeftFootTraj_deq, FinalRightFootTraj_deq);



      if(CurrentSupport.StepsLeft == 0)
        EndingPhase_ = true;
      // Specify that we are in the ending phase.
      if (EndingPhase_==false)
        {
          // This should be done only during the transition EndingPhase=false -> EndingPhase=true
          TimeToStopOnLineMode_ = UpperTimeLimitToUpdate_ + QP_T_ * QP_N_;
          // Set the ZMP reference as very important.
          // It suppose to work because Gamma appears only during the non-constant
        }


      UpperTimeLimitToUpdate_ = UpperTimeLimitToUpdate_+QP_T_;

      // Compute CPU consumption time.
      gettimeofday(&end,0);
      CurrentCPUTime = end.tv_sec - start.tv_sec +
        0.000001 * (end.tv_usec - start.tv_usec);
      TotalAmountOfCPUTime += CurrentCPUTime;


    }

}


void ZMPVelocityReferencedQP::GetZMPDiscretization(deque<ZMPPosition> & ,
						   deque<COMState> & ,
						   deque<RelativeFootPosition> &,
						   deque<FootAbsolutePosition> &,
						   deque<FootAbsolutePosition> &,
						   double ,
						   COMState &,
						   MAL_S3_VECTOR(&,double),
						   FootAbsolutePosition & ,
						   FootAbsolutePosition & )
{
}


void ZMPVelocityReferencedQP::OnLineAddFoot(RelativeFootPosition & ,
					    deque<ZMPPosition> & ,
					    deque<COMState> & ,
					    deque<FootAbsolutePosition> &,
					    deque<FootAbsolutePosition> &,
					    bool)
{
  cout << "To be implemented" << endl;
}

int ZMPVelocityReferencedQP::OnLineFootChange(double ,
					      FootAbsolutePosition &,
					      deque<ZMPPosition> & ,
					      deque<COMState> & ,
					      deque<FootAbsolutePosition> &,
					      deque<FootAbsolutePosition> &,
					      StepStackHandler  *)
{
  cout << "To be implemented" << endl;
  return -1;
}

void ZMPVelocityReferencedQP::EndPhaseOfTheWalking(deque<ZMPPosition> &, 
						   deque<COMState> &,
						   deque<FootAbsolutePosition> &,
						   deque<FootAbsolutePosition> &)
{
  cout << "To be implemented" << endl;
}

int ZMPVelocityReferencedQP::ReturnOptimalTimeToRegenerateAStep()
{
  int r = (int)(m_PreviewControlTime/m_SamplingPeriod);
  return 2*r;
}


