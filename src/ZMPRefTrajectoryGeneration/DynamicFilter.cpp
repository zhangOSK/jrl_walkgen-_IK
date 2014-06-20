#include "DynamicFilter.hh"
#include <metapod/algos/rnea.hh>

using namespace std;
using namespace PatternGeneratorJRL;
using namespace metapod;

DynamicFilter::DynamicFilter(
    SimplePluginManager *SPM,
    CjrlHumanoidDynamicRobot *aHS)
{
  currentTime_ = 0.0 ;
  controlPeriod_ = 0.0 ;
  interpolationPeriod_ = 0.0 ;
  previewWindowSize_ = 0.0 ;
  PG_T_ = 0.0 ;
  NbI_ = 0.0 ;
  NCtrl_ = 0.0;
  NbI_ = 0.0 ;

  comAndFootRealization_ = new ComAndFootRealizationByGeometry(
      (PatternGeneratorInterfacePrivate*) SPM );
  comAndFootRealization_->setHumanoidDynamicRobot(aHS);
  comAndFootRealization_->SetHeightOfTheCoM(CoMHeight_);
  comAndFootRealization_->setSamplingPeriod(interpolationPeriod_);
  comAndFootRealization_->Initialization();

  PC_ = new PreviewControl(
      SPM,OptimalControllerSolver::MODE_WITH_INITIALPOS,false);
  CoMHeight_ = 0.0 ;

  configurationTraj_.clear();
  velocityTraj_.clear();
  accelerationTraj_.clear();
  previousConfiguration_.clear();
  deltaZMP_deq_.clear();
  ZMPMB_vec_.clear();

  MAL_VECTOR_RESIZE(aCoMState_,6);
  MAL_VECTOR_RESIZE(aCoMSpeed_,6);
  MAL_VECTOR_RESIZE(aCoMAcc_,6);
  MAL_VECTOR_RESIZE(aLeftFootPosition_,5);
  MAL_VECTOR_RESIZE(aRightFootPosition_,5);
  MAL_MATRIX_RESIZE(deltax_,3,1);
  MAL_MATRIX_RESIZE(deltay_,3,1);

  previousConfiguration_ = aHS->currentConfiguration() ;
  previousVelocity_ = aHS->currentVelocity() ;
  previousAcceleration_ = aHS->currentAcceleration() ;

  comAndFootRealization_->SetPreviousConfigurationStage0(
      previousConfiguration_);
  comAndFootRealization_->SetPreviousVelocityStage0(
      previousVelocity_);

  Once_ = true ;
  DInitX_ = 0.0 ;
  DInitY_ = 0.0 ;

}

DynamicFilter::~DynamicFilter()
{
  if (PC_!=0){
    delete PC_;
    PC_ = 0 ;
  }
  if (comAndFootRealization_!=0){
    delete comAndFootRealization_;
    comAndFootRealization_ = 0 ;
  }
}

/// \brief Initialse all objects, to be called just after the constructor
void DynamicFilter::init(
    double currentTime,
    double controlPeriod,
    double interpolationPeriod,
    double PG_T,
    double previewWindowSize,
    double CoMHeight,
    FootAbsolutePosition supportFoot)
{
  currentTime_ = currentTime ;
  controlPeriod_ = controlPeriod ;
  interpolationPeriod_ = interpolationPeriod ;
  PG_T_ = PG_T ;
  previewWindowSize_ = previewWindowSize ;

  PreviousSupportFoot_(0,0) = supportFoot.x ;
  PreviousSupportFoot_(1,0) = supportFoot.y ;
  PreviousSupportFoot_(2,0) = supportFoot.z ;
  PreviousSupportFoot_(3,0) = supportFoot.omega ;
  PreviousSupportFoot_(4,0) = supportFoot.omega2 ;
  PreviousSupportFoot_(5,0) = supportFoot.theta ;

  if (interpolationPeriod_>PG_T)
  {NbI_=1;}
  else
  {NbI_ = (int)(PG_T/interpolationPeriod_);}

  NCtrl_ = (int)(PG_T_/controlPeriod_) ;
  PG_N_ = (int)(previewWindowSize_/interpolationPeriod_) ;

  CoMHeight_ = CoMHeight ;
  PC_->SetPreviewControlTime (previewWindowSize_ - PG_T/controlPeriod_ * interpolationPeriod_);
  PC_->SetSamplingPeriod (interpolationPeriod_);
  PC_->SetHeightOfCoM(CoMHeight_);

  previousConfiguration_ = comAndFootRealization_->getHumanoidDynamicRobot()->currentConfiguration() ;
  previousVelocity_ = comAndFootRealization_->getHumanoidDynamicRobot()->currentVelocity() ;
  previousAcceleration_ = comAndFootRealization_->getHumanoidDynamicRobot()->currentAcceleration() ;

  ZMPMBConfiguration_ = comAndFootRealization_->getHumanoidDynamicRobot()->currentConfiguration() ;
  ZMPMBVelocity_ = comAndFootRealization_->getHumanoidDynamicRobot()->currentVelocity() ;
  ZMPMBAcceleration_ = comAndFootRealization_->getHumanoidDynamicRobot()->currentAcceleration() ;

  configurationTraj_.resize( PG_N_, previousConfiguration_ ); ;
  velocityTraj_.resize( PG_N_, previousVelocity_ ); ;
  accelerationTraj_.resize( PG_N_, previousAcceleration_ ); ;

  for(unsigned int j = 0 ; j < ZMPMBConfiguration_.size() ; j++ )
  {
    m_q(j,0) = ZMPMBConfiguration_(j) ;
    m_dq(j,0) = ZMPMBVelocity_(j) ;
    m_ddq(j,0) = ZMPMBAcceleration_(j) ;
  }
  m_prev_q = m_q ;
  m_prev_dq = m_dq ;
  m_prev_ddq = m_ddq ;


  deltaZMP_deq_.resize( PG_N_);
  ZMPMB_vec_.resize( PG_N_, vector<double>(2));

  comAndFootRealization_->setSamplingPeriod(interpolationPeriod_);
  comAndFootRealization_->Initialization();

  MAL_VECTOR_RESIZE(aCoMState_,6);
  MAL_VECTOR_RESIZE(aCoMSpeed_,6);
  MAL_VECTOR_RESIZE(aCoMAcc_,6);
  MAL_VECTOR_RESIZE(aLeftFootPosition_,5);
  MAL_VECTOR_RESIZE(aRightFootPosition_,5);
  MAL_MATRIX_RESIZE(deltax_,3,1);
  MAL_MATRIX_RESIZE(deltay_,3,1);
  return ;
}

int DynamicFilter::filter(
    COMState & lastCtrlCoMState,
    FootAbsolutePosition & lastCtrlLeftFoot,
    FootAbsolutePosition & lastCtrlRightFoot,
    deque<COMState> & inputCOMTraj_deq_,
    deque<ZMPPosition> inputZMPTraj_deq_,
    deque<FootAbsolutePosition> & inputLeftFootTraj_deq_,
    deque<FootAbsolutePosition> & inputRightFootTraj_deq_,
    deque<COMState> & outputDeltaCOMTraj_deq_)
{
  InverseKinematics(
      lastCtrlCoMState,
      lastCtrlLeftFoot,
      lastCtrlRightFoot,
      inputCOMTraj_deq_,
      inputLeftFootTraj_deq_,
      inputRightFootTraj_deq_);

  InverseDynamics(inputZMPTraj_deq_);

  int error = OptimalControl(outputDeltaCOMTraj_deq_);

  printBuffers(inputCOMTraj_deq_,
             inputZMPTraj_deq_,
             inputLeftFootTraj_deq_,
             inputRightFootTraj_deq_,
             outputDeltaCOMTraj_deq_);
  printAlongTime(inputCOMTraj_deq_,
                 inputZMPTraj_deq_,
                 inputLeftFootTraj_deq_,
                 inputRightFootTraj_deq_,
                 outputDeltaCOMTraj_deq_);
  return error ;
}

void DynamicFilter::InverseKinematics(
    COMState & lastCtrlCoMState,
    FootAbsolutePosition & lastCtrlLeftFoot,
    FootAbsolutePosition & lastCtrlRightFoot,
    deque<COMState> & inputCOMTraj_deq_,
    deque<FootAbsolutePosition> & inputLeftFootTraj_deq_,
    deque<FootAbsolutePosition> & inputRightFootTraj_deq_)
{
  int stage0 = 0 ;
  int stage1 = 1 ;

  comAndFootRealization_->SetPreviousConfigurationStage0(previousConfiguration_);
  comAndFootRealization_->SetPreviousVelocityStage0(previousVelocity_);
  comAndFootRealization_->setSamplingPeriod(interpolationPeriod_);
  for(unsigned int i = 0 ; i <  PG_N_ ; i++ )
  {
    InverseKinematics(inputCOMTraj_deq_[i],inputLeftFootTraj_deq_ [i], inputRightFootTraj_deq_ [i],
                      configurationTraj_[i],velocityTraj_[i],accelerationTraj_[i],
                      interpolationPeriod_, stage0, 2);
  }

  InverseKinematics(lastCtrlCoMState, lastCtrlLeftFoot, lastCtrlRightFoot,
                    previousConfiguration_,previousVelocity_,previousAcceleration_,
                    controlPeriod_, stage1, 2);
  return ;
}

void DynamicFilter::InverseKinematics(
    COMState & inputCoMState,
    FootAbsolutePosition & inputLeftFoot,
    FootAbsolutePosition & inputRightFoot,
    MAL_VECTOR_TYPE(double)& configuration,
    MAL_VECTOR_TYPE(double)& velocity,
    MAL_VECTOR_TYPE(double)& acceleration,
    double samplingPeriod,
    int stage,
    int iteration)
{
  aCoMState_(0) = inputCoMState.x[0];      aCoMSpeed_(0) = inputCoMState.x[1];
  aCoMState_(1) = inputCoMState.y[0];      aCoMSpeed_(1) = inputCoMState.y[1];
  aCoMState_(2) = inputCoMState.z[0];      aCoMSpeed_(2) = inputCoMState.z[1];
  aCoMState_(3) = inputCoMState.roll[0];   aCoMSpeed_(3) = inputCoMState.roll[1];
  aCoMState_(4) = inputCoMState.pitch[0];  aCoMSpeed_(4) = inputCoMState.pitch[1];
  aCoMState_(5) = inputCoMState.yaw[0];    aCoMSpeed_(5) = inputCoMState.yaw[1];

  aCoMAcc_(0) = inputCoMState.x[2];    aLeftFootPosition_(0) = inputLeftFoot.x;
  aCoMAcc_(1) = inputCoMState.y[2];    aLeftFootPosition_(1) = inputLeftFoot.y;
  aCoMAcc_(2) = inputCoMState.z[2];    aLeftFootPosition_(2) = inputLeftFoot.z;
  aCoMAcc_(3) = inputCoMState.roll[2]; aLeftFootPosition_(3) = inputLeftFoot.theta;
  aCoMAcc_(4) = inputCoMState.pitch[2];aLeftFootPosition_(4) = inputLeftFoot.omega;
  aCoMAcc_(5) = inputCoMState.yaw[2];

  aRightFootPosition_(0) = inputRightFoot.x;
  aRightFootPosition_(1) = inputRightFoot.y;
  aRightFootPosition_(2) = inputRightFoot.z;
  aRightFootPosition_(3) = inputRightFoot.theta;
  aRightFootPosition_(4) = inputRightFoot.omega;

  comAndFootRealization_->setSamplingPeriod(samplingPeriod);
  comAndFootRealization_->ComputePostureForGivenCoMAndFeetPosture(
      aCoMState_, aCoMSpeed_, aCoMAcc_,
      aLeftFootPosition_, aRightFootPosition_,
      configuration, velocity, acceleration,
      iteration, stage);
  return;
}

void DynamicFilter::InverseDynamics(deque<ZMPPosition> inputZMPTraj_deq)
{
  for (unsigned int i = 0 ; i < PG_N_ ; i++ )
  {
    ComputeZMPMB(configurationTraj_[i],velocityTraj_[i],accelerationTraj_[i],ZMPMB_vec_[i]);

    if (Once_){
      DInitX_ = inputZMPTraj_deq[0].px - ZMPMB_vec_[i][0];
      DInitY_ = inputZMPTraj_deq[0].py - ZMPMB_vec_[i][1];
      Once_ = false ;
    }

    deltaZMP_deq_[i].px = inputZMPTraj_deq[i].px - ZMPMB_vec_[i][0] - DInitX_  ;
    deltaZMP_deq_[i].py = inputZMPTraj_deq[i].py - ZMPMB_vec_[i][1] - DInitY_  ;
    deltaZMP_deq_[i].pz = 0.0 ;
    deltaZMP_deq_[i].theta = 0.0 ;
    deltaZMP_deq_[i].time = currentTime_ + i * interpolationPeriod_ ;
    deltaZMP_deq_[i].stepType = inputZMPTraj_deq[i].stepType ;
  }
  return ;
}

void DynamicFilter::ComputeZMPMB(
    double samplingPeriod,
    COMState  inputCoMState,
    FootAbsolutePosition  inputLeftFoot,
    FootAbsolutePosition  inputRightFoot,
    vector<double> & ZMPMB,
    int iteration)
{
  int stage = 1 ;
  InverseKinematics( inputCoMState, inputLeftFoot, inputRightFoot,
      ZMPMBConfiguration_, ZMPMBVelocity_, ZMPMBAcceleration_,
      samplingPeriod, stage, iteration) ;

  Node & node_waist = boost::fusion::at_c<Robot_Model::BODY>(m_robot.nodes);
  Eigen::Matrix< LocalFloatType, 6, 1 > supportFoot ;
  TransformT<LocalFloatType,Spatial::RotationMatrixIdentityTpl<LocalFloatType> > supportFootXwaist;
  if (inputLeftFoot.stepType<0)
  {
    supportFoot(0,0) = inputLeftFoot.x ;
    supportFoot(1,0) = inputLeftFoot.y ;
    supportFoot(2,0) = inputLeftFoot.z ;
    supportFoot(3,0) = inputLeftFoot.omega ;
    supportFoot(4,0) = inputLeftFoot.omega2 ;
    supportFoot(5,0) = inputLeftFoot.theta ;
    Node & node_lleg0 = boost::fusion::at_c<Robot_Model::LLEG_LINK0>(m_robot.nodes);
    Node & node_lleg1 = boost::fusion::at_c<Robot_Model::LLEG_LINK1>(m_robot.nodes);
    Node & node_lleg2 = boost::fusion::at_c<Robot_Model::LLEG_LINK2>(m_robot.nodes);
    Node & node_lleg3 = boost::fusion::at_c<Robot_Model::LLEG_LINK3>(m_robot.nodes);
    Node & node_lleg4 = boost::fusion::at_c<Robot_Model::LLEG_LINK4>(m_robot.nodes);
    Node & node_l_ankle = boost::fusion::at_c<Robot_Model::l_ankle>(m_robot.nodes);
    supportFootXwaist = node_lleg0.sXp * node_lleg1.sXp * node_lleg2.sXp *
                        node_lleg3.sXp * node_lleg4.sXp * node_l_ankle.sXp ;
  }else
  {
    supportFoot(0,0) = inputRightFoot.x ;
    supportFoot(1,0) = inputRightFoot.y ;
    supportFoot(2,0) = inputRightFoot.z ;
    supportFoot(3,0) = inputRightFoot.omega ;
    supportFoot(4,0) = inputRightFoot.omega2 ;
    supportFoot(5,0) = inputRightFoot.theta ;
    Node & node_rleg0 = boost::fusion::at_c<Robot_Model::RLEG_LINK0>(m_robot.nodes);
    Node & node_rleg1 = boost::fusion::at_c<Robot_Model::RLEG_LINK1>(m_robot.nodes);
    Node & node_rleg2 = boost::fusion::at_c<Robot_Model::RLEG_LINK2>(m_robot.nodes);
    Node & node_rleg3 = boost::fusion::at_c<Robot_Model::RLEG_LINK3>(m_robot.nodes);
    Node & node_rleg4 = boost::fusion::at_c<Robot_Model::RLEG_LINK4>(m_robot.nodes);
    Node & node_r_ankle = boost::fusion::at_c<Robot_Model::r_ankle>(m_robot.nodes);
    supportFootXwaist = node_rleg0.sXp * node_rleg1.sXp * node_rleg2.sXp *
                        node_rleg3.sXp * node_rleg4.sXp * node_r_ankle.sXp ;
  }

  Eigen::Matrix< FloatType, 6, 1 > waist_pos , waist_speed, waist_acc ;
  
  waist_pos =

  // Copy the angular trajectory data from "Boost" to "Eigen"
  for(unsigned int j = 0 ; j < ZMPMBConfiguration_.size() ; j++ )
  {
    m_q(j,0) = ZMPMBConfiguration_(j) ;
    m_dq(j,0) = ZMPMBVelocity_(j) ;
    m_ddq(j,0) = ZMPMBAcceleration_(j) ;
  }

  // Apply the RNEA on the robot model
  metapod::rnea< Robot_Model, true >::run(m_robot, m_q, m_dq, m_ddq);


  m_force = node_waist.body.iX0.applyInv (node_waist.joint.f);

  ZMPMB.resize(2);
  ZMPMB[0] = - m_force.n()[1] / m_force.f()[2] ;
  ZMPMB[1] =   m_force.n()[0] / m_force.f()[2] ;

  PreviousSupportFoot_ = supportFoot ;
  m_prev_q = m_q ;
  m_prev_dq = m_dq ;
  m_prev_ddq = m_ddq ;

  return ;
}

void DynamicFilter::ComputeZMPMB(
    MAL_VECTOR_TYPE(double) & configuration,
    MAL_VECTOR_TYPE(double) & velocity,
    MAL_VECTOR_TYPE(double) & acceleration,
    vector<double> & ZMPMB)
{
  // Copy the angular trajectory data from "Boost" to "Eigen"
  for(unsigned int j = 0 ; j < configuration.size() ; j++ )
  {
    m_q(j,0) = configuration(j) ;
    m_dq(j,0) = velocity(j) ;
    m_ddq(j,0) = acceleration(j) ;
  }

  // Apply the RNEA on the robot model
  metapod::rnea< Robot_Model, true >::run(m_robot, m_q, m_dq, m_ddq);

  Node & node = boost::fusion::at_c<Robot_Model::BODY>(m_robot.nodes);
  m_force = node.body.iX0.applyInv (node.joint.f);

  ZMPMB.resize(2);
  ZMPMB[0] = - m_force.n()[1] / m_force.f()[2] ;
  ZMPMB[1] =   m_force.n()[0] / m_force.f()[2] ;

  return ;
}

int DynamicFilter::OptimalControl(
    deque<COMState> & outputDeltaCOMTraj_deq_)
{
  if(!PC_->IsCoherent())
    PC_->ComputeOptimalWeights(OptimalControllerSolver::MODE_WITH_INITIALPOS);


  double aSxzmp (0) , aSyzmp(0);
  double deltaZMPx (0) , deltaZMPy (0) ;

  // calcul of the preview control along the "deltaZMP_deq_"
  for (unsigned i = 0 ; i < NCtrl_ ; i++ )
  {
    for(int j=0;j<3;j++)
    {
      deltax_(j,0) = 0 ;
      deltay_(j,0) = 0 ;
    }
    PC_->OneIterationOfPreview(deltax_,deltay_,
                               aSxzmp,aSyzmp,
                               deltaZMP_deq_,i,
                               deltaZMPx, deltaZMPy, false);
    for(int j=0;j<3;j++)
    {
      outputDeltaCOMTraj_deq_[i].x[j] = deltax_(j,0);
      outputDeltaCOMTraj_deq_[i].y[j] = deltay_(j,0);
    }
  }

  for (unsigned int i = 0 ; i < NCtrl_ ; i++)
  {
    for(int j=0;j<3;j++)
    {
      if ( outputDeltaCOMTraj_deq_[i].x[j] == outputDeltaCOMTraj_deq_[i].x[j] ||
           outputDeltaCOMTraj_deq_[i].y[j] == outputDeltaCOMTraj_deq_[i].y[j] )
      {}
      else{
        cout << "kajita2003 preview control diverged\n" ;
        return -1 ;
      }
    }
  }
  return 0 ;
}

double DynamicFilter::filterprecision(double adb)
{
  if (fabs(adb)<1e-7)
    return 0.0;

  if (fabs(adb)>1e7)
    return 1e7 ;

  double ladb2 = adb * 1e7;
  double lintadb2 = trunc(ladb2);
  return lintadb2/1e7;
}

void DynamicFilter::printAlongTime(deque<COMState> & inputCOMTraj_deq_,
                               deque<ZMPPosition> inputZMPTraj_deq_,
                               deque<FootAbsolutePosition> & inputLeftFootTraj_deq_,
                               deque<FootAbsolutePosition> & inputRightFootTraj_deq_,
                               deque<COMState> & outputDeltaCOMTraj_deq_)
{  
  // Debug Purpose
  // -------------
  ofstream aof;
  string aFileName;
  ostringstream oss(std::ostringstream::ate);
  static int iteration = 0;

  // --------------------
  oss.str("DynamicFilterAllVariablesNoisyAlongInTime.dat");
  aFileName = oss.str();
  if(iteration == 0)
  {
    aof.open(aFileName.c_str(),ofstream::out);
    aof.close();
  }
  ///----
  aof.open(aFileName.c_str(),ofstream::app);
  aof.precision(8);
  aof.setf(ios::scientific, ios::floatfield);
  aof << filterprecision( iteration*controlPeriod_) << " " // 0
      << filterprecision( inputCOMTraj_deq_[0].x[0] ) << " "    // 1
      << filterprecision( inputCOMTraj_deq_[0].x[1] ) << " "    // 2
      << filterprecision( inputCOMTraj_deq_[0].x[2] ) << " "    // 3
      << filterprecision( inputCOMTraj_deq_[0].y[0] ) << " "    // 4
      << filterprecision( inputCOMTraj_deq_[0].y[1] ) << " "    // 5
      << filterprecision( inputCOMTraj_deq_[0].y[2] ) << " "    // 6
      << filterprecision( inputCOMTraj_deq_[0].z[0] ) << " "    // 7
      << filterprecision( inputCOMTraj_deq_[0].z[1] ) << " "    // 8
      << filterprecision( inputCOMTraj_deq_[0].z[2] ) << " "    // 9
      << filterprecision( inputCOMTraj_deq_[0].roll[0] ) << " " // 10
      << filterprecision( inputCOMTraj_deq_[0].roll[1] ) << " " // 11
      << filterprecision( inputCOMTraj_deq_[0].roll[2] ) << " " // 12
      << filterprecision( inputCOMTraj_deq_[0].pitch[0] ) << " "// 13
      << filterprecision( inputCOMTraj_deq_[0].pitch[1] ) << " "// 14
      << filterprecision( inputCOMTraj_deq_[0].pitch[2] ) << " "// 15
      << filterprecision( inputCOMTraj_deq_[0].yaw[0] ) << " "  // 16
      << filterprecision( inputCOMTraj_deq_[0].yaw[1] ) << " "  // 17
      << filterprecision( inputCOMTraj_deq_[0].yaw[2] ) << " "  // 18

      << filterprecision( inputZMPTraj_deq_[0].px ) << " "      // 19
      << filterprecision( inputZMPTraj_deq_[0].py ) << " "      // 20

      << filterprecision( ZMPMB_vec_[0][0] ) << " "                  // 21
      << filterprecision( ZMPMB_vec_[0][1] ) << " "                  // 22

      << filterprecision( inputLeftFootTraj_deq_[0].x ) << " "       // 23
      << filterprecision( inputLeftFootTraj_deq_[0].y ) << " "       // 24
      << filterprecision( inputLeftFootTraj_deq_[0].z ) << " "       // 25
      << filterprecision( inputLeftFootTraj_deq_[0].theta ) << " "   // 26
      << filterprecision( inputLeftFootTraj_deq_[0].omega ) << " "   // 27
      << filterprecision( inputLeftFootTraj_deq_[0].dx ) << " "      // 28
      << filterprecision( inputLeftFootTraj_deq_[0].dy ) << " "      // 29
      << filterprecision( inputLeftFootTraj_deq_[0].dz ) << " "      // 30
      << filterprecision( inputLeftFootTraj_deq_[0].dtheta ) << " "  // 31
      << filterprecision( inputLeftFootTraj_deq_[0].domega ) << " "  // 32
      << filterprecision( inputLeftFootTraj_deq_[0].ddx ) << " "     // 33
      << filterprecision( inputLeftFootTraj_deq_[0].ddy ) << " "     // 34
      << filterprecision( inputLeftFootTraj_deq_[0].ddz ) << " "     // 35
      << filterprecision( inputLeftFootTraj_deq_[0].ddtheta ) << " " // 36
      << filterprecision( inputLeftFootTraj_deq_[0].ddomega ) << " " // 37

      << filterprecision( inputRightFootTraj_deq_[0].x ) << " "      // 38
      << filterprecision( inputRightFootTraj_deq_[0].y ) << " "      // 39
      << filterprecision( inputRightFootTraj_deq_[0].z ) << " "      // 40
      << filterprecision( inputRightFootTraj_deq_[0].theta ) << " "  // 41
      << filterprecision( inputRightFootTraj_deq_[0].omega ) << " "  // 42
      << filterprecision( inputRightFootTraj_deq_[0].dx ) << " "     // 43
      << filterprecision( inputRightFootTraj_deq_[0].dy ) << " "     // 44
      << filterprecision( inputRightFootTraj_deq_[0].dz ) << " "     // 45
      << filterprecision( inputRightFootTraj_deq_[0].dtheta ) << " " // 46
      << filterprecision( inputRightFootTraj_deq_[0].domega ) << " " // 47
      << filterprecision( inputRightFootTraj_deq_[0].ddx ) << " "    // 48
      << filterprecision( inputRightFootTraj_deq_[0].ddy ) << " "    // 49
      << filterprecision( inputRightFootTraj_deq_[0].ddz ) << " "    // 50
      << filterprecision( inputRightFootTraj_deq_[0].ddtheta ) << " "// 51
      << filterprecision( inputRightFootTraj_deq_[0].ddomega ) << " ";// 52

  for(unsigned int j = 0 ; j < previousConfiguration_.size() ; j++ )
    aof << filterprecision( previousConfiguration_(j) ) << " " ;
  for(unsigned int j = 0 ; j < previousVelocity_.size() ; j++ )
    aof << filterprecision( previousVelocity_(j) ) << " " ;
  for(unsigned int j = 0 ; j < previousAcceleration_.size() ; j++ )
    aof << filterprecision( accelerationTraj_[0](j) ) << " " ;

  aof << filterprecision( outputDeltaCOMTraj_deq_[0].x[0] ) << " "
      << filterprecision( outputDeltaCOMTraj_deq_[0].x[1] ) << " "
      << filterprecision( outputDeltaCOMTraj_deq_[0].x[2] ) << " "
      << filterprecision( outputDeltaCOMTraj_deq_[0].y[0] ) << " "
      << filterprecision( outputDeltaCOMTraj_deq_[0].y[1] ) << " "
      << filterprecision( outputDeltaCOMTraj_deq_[0].y[2] ) << " ";

  aof << endl ;
  aof.close();

  ++iteration;

  return ;
}

void DynamicFilter::printBuffers(deque<COMState> & inputCOMTraj_deq_,
                deque<ZMPPosition> inputZMPTraj_deq_,
                deque<FootAbsolutePosition> & inputLeftFootTraj_deq_,
                deque<FootAbsolutePosition> & inputRightFootTraj_deq_,
                deque<COMState> & outputDeltaCOMTraj_deq_)
{
  // Debug Purpose
  // -------------
  ofstream aof;
  string aFileName;
  ostringstream oss(std::ostringstream::ate);
  static int iteration = 0;
  int iteration100 = (int)iteration/100;
  int iteration10 = (int)(iteration - iteration100*100)/10;
  int iteration1 = (int)(iteration - iteration100*100 - iteration10*10 );

  // --------------------
  oss.str("DumpingData/DynamicFilterAllVariablesAlongInTime_");
  oss << iteration100 << iteration10 << iteration1 << ".dat" ;
  aFileName = oss.str();
  aof.open(aFileName.c_str(),ofstream::out);
  aof.close();

  ///----
  aof.open(aFileName.c_str(),ofstream::app);
  aof.precision(8);
  aof.setf(ios::scientific, ios::floatfield);
  for (unsigned int i = 0 ; i < inputCOMTraj_deq_.size() ; ++i )
  {
    aof << filterprecision( iteration*controlPeriod_ + i*interpolationPeriod_ ) << " " // 0
        << filterprecision( inputCOMTraj_deq_[i].x[0] ) << " "    // 1
        << filterprecision( inputCOMTraj_deq_[i].x[1] ) << " "    // 2
        << filterprecision( inputCOMTraj_deq_[i].x[2] ) << " "    // 3
        << filterprecision( inputCOMTraj_deq_[i].y[0] ) << " "    // 4
        << filterprecision( inputCOMTraj_deq_[i].y[1] ) << " "    // 5
        << filterprecision( inputCOMTraj_deq_[i].y[2] ) << " "    // 6
        << filterprecision( inputCOMTraj_deq_[i].z[0] ) << " "    // 7
        << filterprecision( inputCOMTraj_deq_[i].z[1] ) << " "    // 8
        << filterprecision( inputCOMTraj_deq_[i].z[2] ) << " "    // 9
        << filterprecision( inputCOMTraj_deq_[i].roll[0] ) << " " // 10
        << filterprecision( inputCOMTraj_deq_[i].roll[1] ) << " " // 11
        << filterprecision( inputCOMTraj_deq_[i].roll[2] ) << " " // 12
        << filterprecision( inputCOMTraj_deq_[i].pitch[0] ) << " "// 13
        << filterprecision( inputCOMTraj_deq_[i].pitch[1] ) << " "// 14
        << filterprecision( inputCOMTraj_deq_[i].pitch[2] ) << " "// 15
        << filterprecision( inputCOMTraj_deq_[i].yaw[0] ) << " "  // 16
        << filterprecision( inputCOMTraj_deq_[i].yaw[1] ) << " "  // 17
        << filterprecision( inputCOMTraj_deq_[i].yaw[2] ) << " "  // 18

        << filterprecision( inputZMPTraj_deq_[i].px ) << " "      // 19
        << filterprecision( inputZMPTraj_deq_[i].py ) << " "      // 20

        << filterprecision( inputLeftFootTraj_deq_[i].x ) << " "       // 21
        << filterprecision( inputLeftFootTraj_deq_[i].y ) << " "       // 22
        << filterprecision( inputLeftFootTraj_deq_[i].z ) << " "       // 23
        << filterprecision( inputLeftFootTraj_deq_[i].theta ) << " "   // 24
        << filterprecision( inputLeftFootTraj_deq_[i].omega ) << " "   // 25
        << filterprecision( inputLeftFootTraj_deq_[i].dx ) << " "      // 26
        << filterprecision( inputLeftFootTraj_deq_[i].dy ) << " "      // 27
        << filterprecision( inputLeftFootTraj_deq_[i].dz ) << " "      // 28
        << filterprecision( inputLeftFootTraj_deq_[i].dtheta ) << " "  // 29
        << filterprecision( inputLeftFootTraj_deq_[i].domega ) << " "  // 30
        << filterprecision( inputLeftFootTraj_deq_[i].ddx ) << " "     // 31
        << filterprecision( inputLeftFootTraj_deq_[i].ddy ) << " "     // 32
        << filterprecision( inputLeftFootTraj_deq_[i].ddz ) << " "     // 33
        << filterprecision( inputLeftFootTraj_deq_[i].ddtheta ) << " " // 34
        << filterprecision( inputLeftFootTraj_deq_[i].ddomega ) << " " // 35

        << filterprecision( inputRightFootTraj_deq_[i].x ) << " "       // 36
        << filterprecision( inputRightFootTraj_deq_[i].y ) << " "       // 37
        << filterprecision( inputRightFootTraj_deq_[i].z ) << " "       // 38
        << filterprecision( inputRightFootTraj_deq_[i].theta ) << " "   // 39
        << filterprecision( inputRightFootTraj_deq_[i].omega ) << " "   // 40
        << filterprecision( inputRightFootTraj_deq_[i].dx ) << " "      // 41
        << filterprecision( inputRightFootTraj_deq_[i].dy ) << " "      // 42
        << filterprecision( inputRightFootTraj_deq_[i].dz ) << " "      // 43
        << filterprecision( inputRightFootTraj_deq_[i].dtheta ) << " "  // 44
        << filterprecision( inputRightFootTraj_deq_[i].domega ) << " "  // 45
        << filterprecision( inputRightFootTraj_deq_[i].ddx ) << " "     // 46
        << filterprecision( inputRightFootTraj_deq_[i].ddy ) << " "     // 47
        << filterprecision( inputRightFootTraj_deq_[i].ddz ) << " "     // 48
        << filterprecision( inputRightFootTraj_deq_[i].ddtheta ) << " " // 49
        << filterprecision( inputRightFootTraj_deq_[i].ddomega ) << " ";// 50

    for(unsigned int j = 0 ; j < configurationTraj_[i].size() ; j++ )
      aof << filterprecision( configurationTraj_[i](j) ) << " " ;
    for(unsigned int j = 0 ; j < velocityTraj_[i].size() ; j++ )
      aof << filterprecision( velocityTraj_[i](j) ) << " " ;
    for(unsigned int j = 0 ; j < accelerationTraj_[i].size() ; j++ )
      aof << filterprecision( accelerationTraj_[i](j) ) << " " ;

    aof << filterprecision( ZMPMB_vec_[i][0] ) << " "                  // 159
        << filterprecision( ZMPMB_vec_[i][1] ) << " ";                 // 160

    aof << filterprecision( deltaZMP_deq_[i].px ) << " "                  // 161
        << filterprecision( deltaZMP_deq_[i].py ) << " ";                 // 162


    aof << endl ;
  }
  aof.close();


  static double maxErrX = 0 ;
  static double maxErrY = 0 ;
  for (unsigned int i = 0 ; i < deltaZMP_deq_.size() ; ++i )
  {
    if ( deltaZMP_deq_[i].px > maxErrX )
    {
      maxErrX = deltaZMP_deq_[i].px ;
    }
    if ( deltaZMP_deq_[i].py > maxErrY )
    {
      maxErrY = deltaZMP_deq_[i].py ;
    }
  }

  static double moyErrX = 0 ;
  static double moyErrY = 0 ;
  static double sumErrX = 0 ;
  static double sumErrY = 0 ;
  static int nbRNEAcomputed = 0 ;
  for (unsigned int i = 0 ; i < deltaZMP_deq_.size(); ++i)
  {
    sumErrX += deltaZMP_deq_[i].px ;
    sumErrY += deltaZMP_deq_[i].py ;
  }
  nbRNEAcomputed += deltaZMP_deq_.size() ;
  moyErrX = sumErrX / nbRNEAcomputed ;
  moyErrY = sumErrY / nbRNEAcomputed ;

  aFileName = "TestMorisawa2007OnLine32MoyNoisyZMP.dat" ;
  if(iteration==0){
    aof.open(aFileName.c_str(),ofstream::out);
    aof.close();
  }
  aof.open(aFileName.c_str(),ofstream::app);
  aof.precision(8);
  aof.setf(ios::scientific, ios::floatfield);
  aof << filterprecision(moyErrX ) << " "        // 1
      << filterprecision(moyErrY ) << " "        // 2
      << filterprecision(maxErrX ) << " "        // 3
      << filterprecision(maxErrY ) << " "        // 4
      << endl ;
  aof.close();


  ++iteration;
  return ;
}
