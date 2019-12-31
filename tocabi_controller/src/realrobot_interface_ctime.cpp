#include "tocabi_controller/realrobot_interface.h"
#include "sensoray826/sensoray826.h"

std::mutex mtx_torque_command;
std::mutex mtx_q;

double rising_time = 3.0;
bool elmo_init = true;

RealRobotInterface::RealRobotInterface(DataContainer &dc_global) : dc(dc_global), StateManager(dc_global)
{
    imuSubscriber = dc.nh.subscribe("/imu/data", 1, &RealRobotInterface::ImuCallback, this);
    gainSubscriber = dc.nh.subscribe("/tocabi_controller/gain_command", 100, &RealRobotInterface::gainCallbak, this);

    printf("Starting red ethercat master\n");

    torque_desired.setZero();

    positionElmo.setZero();
    positionExternalElmo.setZero();
    velocityElmo.setZero();
    torqueElmo.setZero();
    torqueDemandElmo.setZero();
    positionDesiredElmo.setZero();
    velocityDesiredElmo.setZero();
    torqueDesiredElmo.setZero();

    torqueDesiredController.setZero();
    for (int i = 0; i < MODEL_DOF; i++)
    {
        dc.currentGain(i) = NM2CNT[i];
    }
}

void RealRobotInterface::updateState()
{
    //State is updated by main state loop of realrobot interface !
    ros::spinOnce();
    if (mtx_q.try_lock())
    {
        q_ = positionElmo;
        q_dot_ = velocityElmo;
        mtx_q.unlock();
        q_virtual_.segment(6, MODEL_DOF) = q_;
        q_dot_virtual_.segment(6, MODEL_DOF) = q_dot_;
    }
}

Eigen::VectorQd RealRobotInterface::getCommand()
{
    mtx_torque_command.lock();
    Eigen::VectorQd ttemp = torqueDesiredController;
    mtx_torque_command.unlock();
    return ttemp;
}

void RealRobotInterface::sendCommand(Eigen::VectorQd command, double sim_time)
{
    if (mtx_torque_command.try_lock())
    {
        torqueDesiredController = command;
        torque_desired = command;
        mtx_torque_command.unlock();
    }
}

void RealRobotInterface::ethercatCheck()
{
    int expectedWKC;
    boolean needlf;
    volatile int wkc;
    boolean inOP;
    uint8 currentgroup = 0;
    printf("S\n");

    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "ethercatCheck Thread Start" << std::endl;
    while (ros::ok())
    {
        if (inOP && ((wkc < expectedWKC) || ec_group[currentgroup].docheckstate))
        {
            printf("S\n");
            if (needlf)
            {
                printf("S\n");
                needlf = FALSE;
                printf("\n");
            }
            // one ore more slaves are not responding
            ec_group[currentgroup].docheckstate = FALSE;
            ec_readstate();
            for (int slave = 1; slave <= ec_slavecount; slave++)
            {
                printf("N %d\n", slave);
                if ((ec_slave[slave].group == currentgroup) && (ec_slave[slave].state != EC_STATE_OPERATIONAL))
                {
                    ec_group[currentgroup].docheckstate = TRUE;
                    if (ec_slave[slave].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR))
                    {
                        printf("ERROR : slave %d is in SAFE_OP + ERROR, attempting ack.\n", slave);
                        ec_slave[slave].state = (EC_STATE_SAFE_OP + EC_STATE_ACK);
                        ec_writestate(slave);
                    }
                    else if (ec_slave[slave].state == EC_STATE_SAFE_OP)
                    {
                        printf("WARNING : slave %d is in SAFE_OP, change to OPERATIONAL.\n", slave);
                        ec_slave[slave].state = EC_STATE_OPERATIONAL;
                        ec_writestate(slave);
                    }
                    else if (ec_slave[slave].state > 0)
                    {
                        if (ec_reconfig_slave(slave, EC_TIMEOUTMON))
                        {
                            ec_slave[slave].islost = FALSE;
                            printf("MESSAGE : slave %d reconfigured\n", slave);
                        }
                    }
                    else if (!ec_slave[slave].islost)
                    {
                        // re-check state
                        ec_statecheck(slave, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                        if (!ec_slave[slave].state)
                        {
                            ec_slave[slave].islost = TRUE;
                            printf("ERROR : slave %d lost\n", slave);
                        }
                    }
                    if (ElmoConnected)
                        std::cout << "WARNING!!!! EC STATE is Not Operational!!!!! Please Check ! " << std::endl;
                }
                if (ec_slave[slave].islost)
                {
                    if (!ec_slave[slave].state)
                    {
                        if (ec_recover_slave(slave, EC_TIMEOUTMON))
                        {
                            ec_slave[slave].islost = FALSE;
                            printf("MESSAGE : slave %d recovered\n", slave);
                        }
                    }
                    else
                    {
                        ec_slave[slave].islost = FALSE;
                        printf("MESSAGE : slave %d found\n", slave);
                    }
                }
            }
            if (!ec_group[currentgroup].docheckstate)
                printf("*");
        }

        /*
        for (int slave = 1; slave <= ec_slavecount; slave++)
            std::cout << "slave : " << slave << "\t" << ec_slave[slave].state << "\t";

        std::cout << std::endl;
*/
        std::this_thread::sleep_for(std::chrono::microseconds(250));
        if (ElmoTerminate)
        {
            dc.shutdown = true;
            break;
        }
    }
    std::cout << "checking thread end !" << std::endl;
}
void RealRobotInterface::ethercatThread()
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "ethercatThread Start" << std::endl;

    char IOmap[4096];
    bool reachedInitial[MODEL_DOF] = {false};

    const char *ifname = dc.ifname.c_str();

    struct timespec current, begin, time;
    double elapsed = 0.0, elapsed_sum = 0.0, elapsed_avg = 0.0, elapsed_var = 0.0, prev = 0.0, now = 0.0, current_time = 0.0, begin_time = 0.0;
    double elapsed_time[10000] = {0.0};
    static int elapsed_cnt = 0, max_cnt = 0, min_cnt = 0;
    double elapsed_min = 210000000.0, elapsed_max = 0.0;
    double time_mem[10000] = {0.0};

    if (ec_init(ifname))
    {
        printf("ec_init on %s succeeded.\n", ifname);

        /* find and auto-config slaves */
        /* network discovery */
        if (ec_config_init(FALSE) > 0) // TRUE when using configtable to init slaves, FALSE otherwise
        {
            printf("%d slaves found and configured.\n", ec_slavecount); // ec_slavecount -> slave num

            /** CompleteAccess disabled for Elmo driver */
            for (int slave = 1; slave <= ec_slavecount; slave++)
            {
                printf("Has Slave[%d] CA? %s\n", slave, ec_slave[slave].CoEdetails & ECT_COEDET_SDOCA ? "true" : "false");
                ec_slave[slave].CoEdetails ^= ECT_COEDET_SDOCA;
            }
            ec_statecheck(0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);

            if (2 == ec_slavecount)
            {

                for (int slave = 1; slave <= ec_slavecount; slave++)
                {
                    uint16 map_1c12[2] = {0x0001, 0x1605};
                    uint16 map_1c13[6] = {0x0003, 0x1a00, 0x1a11, 0x1a13};//, 0x1a1e, 0X1a1c};
                    int os;
                    os = sizeof(map_1c12);
                    ec_SDOwrite(slave, 0x1c12, 0, TRUE, os, map_1c12, EC_TIMEOUTRXM);
                    os = sizeof(map_1c13);
                    ec_SDOwrite(slave, 0x1c13, 0, TRUE, os, map_1c13, EC_TIMEOUTRXM);
                }

                /** if CA disable => automapping works */
                ec_config_map(&IOmap);

                /* wait for all slaves to reach SAFE_OP state */
                ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

                printf("Request operational state for all slaves\n");
                int expectedWKC = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;
                printf("Calculated workcounter %d\n", expectedWKC);

                /** going operational */
                ec_slave[0].state = EC_STATE_OPERATIONAL;

                /* send one valid process data to make outputs in slaves happy*/
                ec_send_processdata();
                ec_receive_processdata(EC_TIMEOUTRET);

                /* request OP state for all slaves */
                ec_writestate(0);

                int wait_cnt = 40;

                /* wait for all slaves to reach OP state */
                do
                {
                    ec_send_processdata();
                    ec_receive_processdata(EC_TIMEOUTRET);
                    ec_statecheck(0, EC_STATE_OPERATIONAL, 5000);
                } while (wait_cnt-- && (ec_slave[0].state != EC_STATE_OPERATIONAL));

                if (ec_slave[0].state == EC_STATE_OPERATIONAL)
                {
                    //printf("Operational state reached for all slaves.\n");
                    rprint(dc, 15, 5, "Operational state reached for all slaves.");
                    rprint(dc, 16, 5, "Starting red controller threads in ... 3");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    rprint(dc, 1, 1, "2...");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    rprint(dc, 1, 1, "1...");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    rprint(dc, 1, 1, "0...Start!");

                    dc.connected = true;
                    /* cyclic loop */
                    for (int slave = 1; slave <= ec_slavecount; slave++)
                    {
                        txPDO[slave - 1] = (EtherCAT_Elmo::ElmoGoldDevice::elmo_gold_tx *)(ec_slave[slave].outputs);
                        rxPDO[slave - 1] = (EtherCAT_Elmo::ElmoGoldDevice::elmo_gold_rx *)(ec_slave[slave].inputs);
                    }
                    
                    std::chrono::high_resolution_clock::time_point t_begin = std::chrono::high_resolution_clock::now();
                    std::chrono::duration<double> time_from_begin;
                    std::chrono::high_resolution_clock::time_point t1;
                    std::chrono::high_resolution_clock::time_point t2;
                    std::chrono::high_resolution_clock::time_point t3;
                    std::chrono::high_resolution_clock::time_point t4;
                    std::chrono::high_resolution_clock::time_point t5;
                    std::chrono::high_resolution_clock::time_point t6;

                    t4=std::chrono::high_resolution_clock::now();

                    std::chrono::microseconds mc(500);
                    std::chrono::duration<double> tptp;
                    std::chrono::duration<double> tptp2;
                    /*
                    std::chrono::microseconds cycletime(dc.ctime);
                    int cycle_count = 1;

                    std::chrono::high_resolution_clock::time_point t1;
                    std::chrono::high_resolution_clock::time_point t2;
                    std::chrono::high_resolution_clock::time_point t3;*/
                    double to_ratio, to_calib;


                    struct timespec ts;
                    int64 cycletime;

                    cycletime = dc.ctime* 1000; /* cycletime in ns */

                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    clock_gettime(CLOCK_MONOTONIC, &begin);
                    prev = begin.tv_sec;
                    prev += begin.tv_nsec / 1000000000.0;

                    while (1)
                    {
                        //Ethercat Loop begins :: RT Thread

                        //std::this_thread::sleep_until(t_begin + cycle_count * cycletime);
                        
                        t1 = std::chrono::high_resolution_clock::now();
                        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

                        //t1 = std::chrono::high_resolution_clock::now();
                        time_from_begin = t1 - t_begin;
                        control_time_ = time_from_begin.count();
                        /** PDO I/O refresh */
                        ec_send_processdata();

                        t6 = std::chrono::high_resolution_clock::now();
                        wkc = ec_receive_processdata(250);

                        t2 = std::chrono::high_resolution_clock::now();
                        if (wkc >= expectedWKC)
                        {

                            for (int slave = 1; slave <= ec_slavecount; slave++)
                            {
                                if (controlWordGenerate(rxPDO[slave - 1]->statusWord, txPDO[slave - 1]->controlWord))
                                {
                                    reachedInitial[slave - 1] = true;
                                }
                            }

                            torqueDesiredElmo = getCommand();
                            positionDesiredElmo = dc.positionDesired;
                            for (int slave = 1; slave <= ec_slavecount; slave++)
                            {
                                if (reachedInitial[slave - 1])
                                {
                                    //Get status
                                    positionElmo(slave - 1) = rxPDO[slave - 1]->positionActualValue * CNT2RAD[slave - 1] * Dr[slave - 1];

                                    velocityElmo(slave - 1) =
                                        (((int32_t)ec_slave[slave].inputs[10]) +
                                         ((int32_t)ec_slave[slave].inputs[11] << 8) +
                                         ((int32_t)ec_slave[slave].inputs[12] << 16) +
                                         ((int32_t)ec_slave[slave].inputs[13] << 24)) *
                                        CNT2RAD[slave - 1] * Dr[slave - 1];
/*
                                    torqueDemandElmo(slave - 1) =
                                        (int16_t)((ec_slave[slave].inputs[18]) +
                                                  (ec_slave[slave].inputs[19] << 8)) *
                                        Dr[slave - 1];

                                    positionExternalElmo(slave - 1) =
                                        (((int32_t)ec_slave[slave].inputs[20]) +
                                         ((int32_t)ec_slave[slave].inputs[21] << 8) +
                                         ((int32_t)ec_slave[slave].inputs[22] << 16) +
                                         ((int32_t)ec_slave[slave].inputs[23] << 24)) *
                                        CNT2RAD[slave - 1] * Dr[slave - 1];

                                    hommingElmo[slave - 1] =
                                        (((uint32_t)ec_slave[slave].inputs[24]) +
                                         ((uint32_t)ec_slave[slave].inputs[25] << 8) +
                                         ((uint32_t)ec_slave[slave].inputs[26] << 16) +
                                         ((uint32_t)ec_slave[slave].inputs[27] << 24));

  */
//                                  torqueElmo(slave - 1) = rxPDO[slave - 1]->torqueActualValue * Dr[slave - 1];

                                    ElmoConnected = true;
                                    if (elmo_init)
                                    {
                                        positionInitialElmo = positionElmo;
                                        if (control_time_ > 1.0)
                                            elmo_init = false;

                                        txPDO[slave - 1]->modeOfOperation = EtherCAT_Elmo::CyclicSynchronousTorquemode;
                                        txPDO[slave - 1]->targetTorque = 0.0;
                                    }

                                    txPDO[slave - 1]->maxTorque = (uint16)200; // originaly 1000

                                    dc.torqueElmo = torqueElmo;

                                    //Get status END

                                    //txPDO[slave - 1]->modeOfOperation = EtherCAT_Elmo::CyclicSynchronousPositionmode;

                                    //mtx_q.lock();
                                    //txPDO[slave - 1]->targetPosition = (positionDesiredElmo(slave - 1)) * RAD2CNT[slave - 1]* Dr[slave - 1];
                                    //txPDO[slave - 1]->targetTorque = (int)(torqueDesiredElmo(slave - 1) * NM2CNT[slave - 1] * Dr[slave - 1]);
                                    //mtx_q.unlock();
                                    //std::cout << ec_slave[0].state << std::endl;

                                    if (!dc.elmo_Ready)
                                    {
                                        if (!elmo_init)
                                        {
                                            //Homming test for R7 +20 -20 for
                                            if (slave == 2)
                                            {
                                                txPDO[slave - 1]->modeOfOperation = EtherCAT_Elmo::CyclicSynchronousPositionmode;
                                                txPDO[slave - 1]->targetPosition = (int)(positionInitialElmo(slave - 1) * RAD2CNT[slave - 1] * Dr[slave - 1]);
                                                //std::cout<<"commanding ... "<<control_time_<<std::endl;

//                                                txPDO[slave - 1]->modeOfOperation = EtherCAT_Elmo::CyclicSynchronousTorquemode;
  //                                              txPDO[slave - 1]->targetTorque = 100;
                                                
                                            }
                                            else
                                            {
                                                txPDO[slave - 1]->modeOfOperation = EtherCAT_Elmo::CyclicSynchronousTorquemode;
                                                txPDO[slave - 1]->targetTorque = (int)0;
                                            }
                                        }
                                    }
                                    else if (dc.emergencyoff)
                                    {
                                        txPDO[slave - 1]->targetTorque = 0.0;
                                    }
                                    else
                                    {
                                        if (dc.torqueOn)
                                        {
                                            //If torqueOn command received, torque will increases slowly, for rising_time, which is currently 3 seconds.
                                            to_ratio = DyrosMath::minmax_cut((control_time_ - dc.torqueOnTime) / rising_time, 0.0, 1.0);
                                            dc.t_gain = to_ratio;
                                            if (dc.positionControl)
                                            {
                                                torqueDesiredElmo(slave - 1) = (Kp[slave - 1] * (positionDesiredElmo(slave - 1) - positionElmo(slave - 1))) + (Kv[slave - 1] * (0 - velocityElmo(slave - 1)));
                                                txPDO[slave - 1]->targetTorque = (int)(to_ratio * torqueDesiredElmo(slave - 1) * Dr[slave - 1]);
                                            }
                                            else
                                            {
                                                if (dc.customGain)
                                                {
                                                    txPDO[slave - 1]->targetTorque = (int)(to_ratio * torqueDesiredElmo(slave - 1) / CustomGain[slave - 1] * Dr[slave - 1]);
                                                }
                                                else
                                                {
                                                    txPDO[slave - 1]->targetTorque = (int)(to_ratio * torqueDesiredElmo(slave - 1) / NM2CNT[slave - 1] * Dr[slave - 1]);
                                                }
                                            }
                                        }
                                        else if (dc.torqueOff)
                                        {
                                            //If torqueOff command received, torque will decreases slowly, for rising_time(3 seconds. )

                                            if (dc.torqueOnTime + rising_time > dc.torqueOffTime)
                                            {
                                                to_calib = (dc.torqueOffTime - dc.torqueOnTime) / rising_time;
                                            }
                                            else
                                            {
                                                to_calib = 0.0;
                                            }
                                            to_ratio = DyrosMath::minmax_cut(1.0 - to_calib - (control_time_ - dc.torqueOffTime) / rising_time, 0.0, 1.0);

                                            dc.t_gain = to_ratio;

                                            if (dc.positionControl)
                                            {
                                                torqueDesiredElmo(slave - 1) = (Kp[slave - 1] * (positionDesiredElmo(slave - 1) - positionElmo(slave - 1))) + (Kv[slave - 1] * (0 - velocityElmo(slave - 1)));
                                                txPDO[slave - 1]->targetTorque = (int)(to_ratio * torqueDesiredElmo(slave - 1) * Dr[slave - 1]);
                                            }
                                            else
                                            {
                                                txPDO[slave - 1]->targetTorque = (int)(to_ratio * torqueDesiredElmo(slave - 1) / NM2CNT[slave - 1] * Dr[slave - 1]);
                                            }
                                        }
                                    }
                                }
                            }
                        }


                        if((t3- t1)>mc)
                        {
                            std::cout<<"Warning! time over!"<<std::endl;
                        }
                        if (dc.shutdown || !ros::ok())
                        {
                            ElmoTerminate = true;
                            //std::terminate();
                            break;
                        }
                        clock_gettime(CLOCK_MONOTONIC, &time);
                        now = time.tv_sec;
                        now += time.tv_nsec / 1000000000.0;
                        elapsed_time[elapsed_cnt] = now - prev;
                        prev = now;

                    
                        elapsed_sum += elapsed_time[elapsed_cnt];
                        if (elapsed_min > elapsed_time[elapsed_cnt])
                            elapsed_min = elapsed_time[elapsed_cnt];
                        if (elapsed_max < elapsed_time[elapsed_cnt])
                            elapsed_max = elapsed_time[elapsed_cnt];

                        time_mem[elapsed_cnt] = (elapsed_time[elapsed_cnt] - (cycletime / 1000000000.0)) * 1000;

                        if (++elapsed_cnt >= 100)
                        {
                            elapsed_avg = elapsed_sum / elapsed_cnt;
                            for (int i = 0; i < elapsed_cnt; i++)
                            {
                                elapsed_var += (elapsed_time[i] - elapsed_avg) * (elapsed_time[i] - elapsed_avg);
                                if (elapsed_time[i] > elapsed_avg + 0.00010)
                                    max_cnt++;
                                if (elapsed_time[i] < elapsed_avg - 0.00010)
                                    min_cnt++;
                            }

                            elapsed_var = elapsed_var / elapsed_cnt;
                            printf("avg = %.3lf\tmin = %.3lf\tmax = %.3lf\tvar = %.6lf\tmax_cnt=%d\tmin_cnt=%d\tcnt = %d\n", elapsed_avg*1000, elapsed_min*1000, elapsed_max*1000, elapsed_var*1000000, max_cnt, min_cnt, elapsed_cnt);
                            //printf("torqued %d %d \n",D_torque,torqueDemandElmo(1));

                            max_cnt = 0;
                            min_cnt = 0;
                            elapsed_sum = 0;
                            elapsed_var = 0;
                            elapsed_cnt = 0;
                            elapsed_min = 210000000.0;
                            elapsed_max = 0.0;
                        }

                        add_timespec(&ts, cycletime);


                        tptp2 = t1-t5;
                        t3= std::chrono::high_resolution_clock::now();

                        tptp = t3-t4;
                        double dt1= tptp.count();

                        tptp= t6-t1;
                        
                        if(dt1*1000>5)
                        {
                            std::cout<<"!!!! Time check : "<<dt1*1000<<"\tloop check !"<<tptp.count()*1000;
                            tptp=t2-t6;
                            std::cout<<"\tloop check 2 : " << tptp.count()*1000<<"\tloop check 3 : "<<tptp2.count()*1000<<std::endl;
                            
                        }

                        t4=t3;
                        
                        t5= std::chrono::high_resolution_clock::now();
                    }
                }
                else
                {
                    printf("Not all slaves reached operational state.\n");
                    ec_readstate();
                    for (int slave = 1; slave <= ec_slavecount; slave++)
                    {
                        if (ec_slave[slave - 1].state != EC_STATE_OPERATIONAL)
                        {
                            printf("EtherCAT State Operation Error : Slave %d State=0x%2.2x StatusCode=0x%4.4x : %s\n",
                                   slave - 1, ec_slave[slave - 1].state, ec_slave[slave - 1].ALstatuscode, ec_ALstatuscode2string(ec_slave[slave - 1].ALstatuscode));
                        }
                    }
                }
                printf("\nRequest init state for all slaves\n");
                /** request INIT state for all slaves
             *  slave number = 0 -> write to all slaves
             */
                ec_slave[0].state = EC_STATE_INIT;
                ec_writestate(0);
            }
            else
            {
                printf("Ethercat Slave Count insufficient ! model_dof : %d , ec slave count : %d\n", MODEL_DOF, ec_slavecount);
                ElmoTerminate = true;
            }
        }
        else
        {
            printf("No slaves found!\n");
            ElmoTerminate = true;
        }
    }
    else
    {
        printf("No socket connection on %s\nExcecute as root\n", ifname);
        ElmoTerminate = true;
    }

    std::cout << "Ethercat Thread End!" << std::endl;
    ElmoTerminate = true;
}
void RealRobotInterface::imuThread()
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::chrono::high_resolution_clock::time_point t_begin = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_from_begin;

    std::chrono::microseconds cycletime(1000);

    int cycle_count = 0;
    while (!dc.shutdown)
    {
        std::this_thread::sleep_until(t_begin + cycle_count * cycletime);
        cycle_count++;
        //Code here
        //
    }

    std::cout << "IMU Thread End!" << std::endl;
}
void RealRobotInterface::ftsensorThread()
{
    //wait for
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::chrono::high_resolution_clock::time_point t_begin = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> time_from_begin;

    std::chrono::microseconds cycletime(1000);

    int cycle_count = 0;

    sensoray826_dev ft = sensoray826_dev(0);
    ft.analogSingleSamplePrepare(slotAttrs, 16);
    ft.initCalibration();

    while (!dc.shutdown)
    {
        std::this_thread::sleep_until(t_begin + cycle_count * cycletime);
        cycle_count++;

        ft.analogOversample();
        ft.computeFTData();

        rprint(dc, "FTsensor x : %f \t y : %f \t z : %f", ft.leftFootAxisData[0], ft.leftFootAxisData[1], ft.leftFootAxisData[2]);
    }

    std::cout << "FTsensor Thread End!" << std::endl;
}

void RealRobotInterface::ImuCallback(const sensor_msgs::ImuConstPtr &msg)
{
    q_virtual_(3) = msg->orientation.x;
    q_virtual_(4) = msg->orientation.y;
    q_virtual_(5) = msg->orientation.z;

    q_virtual_(MODEL_DOF_VIRTUAL) = msg->orientation.w;

    q_dot_virtual_(3) = msg->angular_velocity.x;
    q_dot_virtual_(4) = msg->angular_velocity.y;
    q_dot_virtual_(5) = msg->angular_velocity.z;

    //62.8hz lowpass velocity
}

bool RealRobotInterface::controlWordGenerate(const uint16_t statusWord, uint16_t &controlWord)
{
    if (!(statusWord & (1 << OPERATION_ENABLE_BIT)))
    {
        if (!(statusWord & (1 << SWITCHED_ON_BIT)))
        {
            if (!(statusWord & (1 << READY_TO_SWITCH_ON_BIT)))
            {
                if (statusWord & (1 << FAULT_BIT))
                {
                    controlWord = 0x80;
                    return false;
                }
                else
                {
                    controlWord = CW_SHUTDOWN;
                    return false;
                }
            }
            else
            {
                controlWord = CW_SWITCHON;
                return true;
            }
        }
        else
        {
            controlWord = CW_ENABLEOP;
            return true;
        }
    }
    else
    {
        controlWord = CW_ENABLEOP;
        return true;
    }
    controlWord = 0;
    return false;
}

void RealRobotInterface::add_timespec(struct timespec *ts, int64 addtime)
{
    int64 sec, nsec;

    nsec = addtime % NSEC_PER_SEC;
    sec = (addtime - nsec) / NSEC_PER_SEC;
    ts->tv_sec += sec;
    ts->tv_nsec += nsec;
    if (ts->tv_nsec > NSEC_PER_SEC)
    {
        nsec = ts->tv_nsec % NSEC_PER_SEC;
        ts->tv_sec += (ts->tv_nsec - nsec) / NSEC_PER_SEC;
        ts->tv_nsec = nsec;
    }
}

void RealRobotInterface::gainCallbak(const tocabi_controller::GainCommandConstPtr &msg)
{

    std::cout << "customgain Command received ! " << std::endl;
    for (int i = 0; i < MODEL_DOF; i++)
    {
        CustomGain[i] = msg->gain[i];
        std::cout << CustomGain[i] << "\t";
    }
    std::cout << std::endl;
    dc.customGain = true;
}
