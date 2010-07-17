// -*- coding: utf-8 -*-
// Copyright (C) 2006-2010 Rosen Diankov (rdiankov@cs.cmu.edu)
//
// This file is part of OpenRAVE.
// OpenRAVE is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#ifndef RAVE_ENVIRONMENT_H
#define RAVE_ENVIRONMENT_H

#include <locale>

#ifdef HAVE_BOOST_FILESYSTEM
#include <boost/filesystem/operations.hpp>
#endif

#define GRAPH_DELETER boost::bind(&Environment::_CloseGraphCallback,boost::static_pointer_cast<Environment>(shared_from_this()),RaveViewerBaseWeakPtr(_pCurrentViewer),_1)

#define CHECK_INTERFACE(pinterface) { \
        if( (pinterface)->GetEnv() != shared_from_this() ) \
            throw openrave_exception(str(boost::format("Interface %s is from a different environment")%(pinterface)->GetXMLId()),ORE_InvalidArguments); \
    } \

#define CHECK_COLLISION_BODY(body) { \
        CHECK_INTERFACE(body); \
        if( !(body)->GetCollisionData() ) { \
            RAVELOG_WARNA("body %s not added to enviornment!\n", (body)->GetName().c_str()); \
            return false; \
        } \
    }

class Environment : public EnvironmentBase
{
 public:
    Environment(bool bLoadAllPlugins=true) : EnvironmentBase()
    {
        // set to the classic locale so that number serialization/hashing works correctly
        std::locale::global(std::locale::classic());
        _homedirectory = RaveGetHomeDirectory();
        RAVELOG_DEBUGA("setting openrave cache directory to %s\n",_homedirectory.c_str());

        _nBodiesModifiedStamp = 0;
        _nEnvironmentIndex = 1;

        _fDeltaSimTime = 0.01f;
        _nCurSimTime = 0;
        _nSimStartTime = GetMicroTime();
        _bRealTime = true;

        _bDestroying = true;
        _bDestroyed = false;
        _bEnableSimulation = true; // need to start by default
    
        _pdatabase.reset(new RaveDatabase());
        if( bLoadAllPlugins ) {
            ParseDirectories(getenv("OPENRAVE_PLUGINS"), _vplugindirs);
            bool bExists=false;
#ifdef HAVE_BOOST_FILESYSTEM
            boost::filesystem::path pluginsfilename = boost::filesystem::system_complete(boost::filesystem::path(OPENRAVE_PLUGINS_INSTALL_DIR, boost::filesystem::native));
            FOREACH(itname, _vplugindirs) {
                if( pluginsfilename == boost::filesystem::system_complete(boost::filesystem::path(*itname, boost::filesystem::native)) ) {
                    bExists = true;
                    break;
                }
            }
#else
            string pluginsfilename=OPENRAVE_PLUGINS_INSTALL_DIR;
            FOREACH(itname, _vplugindirs) {
                if( pluginsfilename == OPENRAVE_PLUGINS_INSTALL_DIR ) {
                    bExists = true;
                    break;
                }
            }
#endif
            if( !bExists ) {
                _vplugindirs.push_back(OPENRAVE_PLUGINS_INSTALL_DIR);
            }
        }

        {
            ParseDirectories(getenv("OPENRAVE_DATA"), _vdatadirs);
            bool bExists=false;
#ifdef HAVE_BOOST_FILESYSTEM
            boost::filesystem::path datafilename = boost::filesystem::system_complete(boost::filesystem::path(OPENRAVE_DATA_INSTALL_DIR, boost::filesystem::native));
            FOREACH(itname, _vplugindirs) {
                if( datafilename == boost::filesystem::system_complete(boost::filesystem::path(*itname, boost::filesystem::native)) ) {
                    bExists = true;
                    break;
                }
            }
#else
            string datafilename=OPENRAVE_DATA_INSTALL_DIR;
            FOREACH(itname, _vplugindirs) {
                if( datafilename == OPENRAVE_DATA_INSTALL_DIR ) {
                    bExists = true;
                    break;
                }
            }
#endif
            if( !bExists ) {
                _vdatadirs.push_back(OPENRAVE_DATA_INSTALL_DIR);
            }
       }
    }

    virtual ~Environment() {
        Destroy();
    }

    virtual void Init()
    {
        _bDestroying = false;
        if( !_pCurrentChecker )
            _pCurrentChecker.reset(new DummyCollisionChecker(shared_from_this()));
        if( !_pCurrentViewer )
            _pCurrentViewer.reset(new DummyRaveViewer(shared_from_this()));
        if( !_pPhysicsEngine )
            _pPhysicsEngine.reset(new DummyPhysicsEngine(shared_from_this()));

        FOREACH(it, _vplugindirs) {
            if( it->size() > 0 )
                _pdatabase->AddDirectory(it->c_str());
        }

        // set a collision checker, don't call EnvironmentBase::CreateCollisionChecker
        CollisionCheckerBasePtr localchecker;
        boost::array<string,3> checker_prefs = {{"ode", "bullet", "pqp"}}; // ode takes priority since bullet has some bugs with deleting bodies
        FOREACH(itchecker,checker_prefs) {
            localchecker = _pdatabase->CreateCollisionChecker(shared_from_this(), *itchecker);
            if( !!localchecker )
                break;
        }

        if( !localchecker ) { // take any collision checker
            std::list<RaveDatabase::PluginPtr> listplugins; _pdatabase->GetPlugins(listplugins);
            FOREACHC(itplugin, listplugins) {
                PLUGININFO info;
                if( !(*itplugin)->GetInfo(info) ) {
                    continue;
                }
                std::map<InterfaceType, std::vector<std::string> >::const_iterator itnames =info.interfacenames.find(PT_CollisionChecker);
                if( itnames != info.interfacenames.end() ) {
                    FOREACHC(itname, itnames->second) {
                        localchecker = _pdatabase->CreateCollisionChecker(shared_from_this(), *itname);
                        if( !!localchecker )
                            break;
                    }

                    if( !!localchecker )
                        break;
                }
            }
        }

        if( !!localchecker ) {
            RAVELOG_DEBUGA("using %s collision checker\n", localchecker->GetXMLId().c_str());
            SetCollisionChecker(localchecker);
        }
        else
            RAVELOG_WARNA("failed to find any collision checker.\n");

        if( !!_threadSimulation )
            _threadSimulation->join();
        _threadSimulation.reset(new boost::thread(boost::bind(&Environment::_SimulationThread,this)));
    }

    virtual void Destroy()
    {
        // destruction order is *very* important, don't touch it without consultation
        _bDestroying = true;

        // dont' join, might not return
        RAVELOG_DEBUGA("Environment destructor\n");
        if( !!_threadSimulation )
            _threadSimulation->join();
        _threadSimulation.reset();

        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        _bEnableSimulation = false;

        RAVELOG_DEBUGA("resetting raveviewer\n");
        if( !!_pCurrentViewer ) {
            _pCurrentViewer->deselect();
            _pCurrentViewer->Reset();
            _pCurrentViewer->quitmainloop();
        }
    
        if( !!_pPhysicsEngine )
            _pPhysicsEngine->DestroyEnvironment();
        if( !!_pCurrentChecker )
            _pCurrentChecker->DestroyEnvironment();

        {
            boost::mutex::scoped_lock lock(_mutexBodies);
            // release all grabbed
            FOREACH(itrobot,_vecrobots)
                (*itrobot)->ReleaseAllGrabbed();
            FOREACH(itbody,_vecbodies)
                (*itbody)->Destroy();
            _vecbodies.clear();
            FOREACH(itrobot,_vecrobots)
                (*itrobot)->Destroy();
            _vecrobots.clear();
            _vPublishedBodies.clear();
            _nBodiesModifiedStamp++;
        }

        RAVELOG_DEBUGA("destroy problems\n");
        list<ProblemInstancePtr> listProblems;
        {
            boost::mutex::scoped_lock lock(_mutexProblems);
            listProblems.swap(_listProblems);
        }
        
        FOREACH(itprob,listProblems)
            (*itprob)->Destroy();
        listProblems.clear();
        _listOwnedObjects.clear();

        _pCurrentChecker.reset();
        _pPhysicsEngine.reset();
        _pCurrentViewer.reset();
        _pdatabase.reset();
    }

    virtual void Reset()
    {
        // destruction order is *very* important, don't touch it without consultation

        RAVELOG_DEBUGA("resetting raveviewer\n");
        if( !!_pCurrentViewer ) {
            _pCurrentViewer->deselect();
            _pCurrentViewer->Reset();
        }
    
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        
        if( !!_pPhysicsEngine )
            _pPhysicsEngine->DestroyEnvironment();
        if( !!_pCurrentChecker )
            _pCurrentChecker->DestroyEnvironment();

        {
            boost::mutex::scoped_lock lock(_mutexBodies);
            boost::mutex::scoped_lock locknetworkid(_mutexEnvironmentIds);

            FOREACH(itbody,_vecbodies) {
                (*itbody)->_environmentid=0;
                (*itbody)->Destroy();
            }
            _vecbodies.clear();
            FOREACH(itrobot,_vecrobots) {
                (*itrobot)->_environmentid=0;
                (*itrobot)->Destroy();
            }
            _vecrobots.clear();
            _vPublishedBodies.clear();
            _nBodiesModifiedStamp++;
            
            _mapBodies.clear();
        }

        list<ProblemInstancePtr> listProblems;
        {
            boost::mutex::scoped_lock lock(_mutexProblems);
            listProblems = _listProblems;
        }

        FOREACH(itproblem,listProblems)
            (*itproblem)->Reset();
        listProblems.clear();

//        {
//            boost::mutex::scoped_lock locknetworkid(_mutexEnvironmentIds);
//            _mapBodies.clear();
//            _nEnvironmentIndex = 1;
//        }
        _listOwnedObjects.clear();

        // load the dummy physics engine
        SetPhysicsEngine(PhysicsEngineBasePtr());

        if( !!_pCurrentChecker )
            _pCurrentChecker->InitEnvironment();
        if( !!_pPhysicsEngine )
            _pPhysicsEngine->InitEnvironment();
    }

    virtual void GetPluginInfo(std::list< std::pair<std::string, PLUGININFO> >& plugins)
    {
        plugins.clear();
        list<RaveDatabase::PluginPtr> listdbplugins;
        _pdatabase->GetPlugins(listdbplugins);
        FOREACHC(itplugin, listdbplugins) {
            PLUGININFO info;
            if( (*itplugin)->GetInfo(info) ) {
                plugins.push_back(pair<string,PLUGININFO>((*itplugin)->GetName(),info));
            }
        }
    }

    virtual void GetLoadedInterfaces(PLUGININFO& info)
    {
        info.interfacenames.clear();
        list<RaveDatabase::PluginPtr> listdbplugins;
        _pdatabase->GetPlugins(listdbplugins);
        FOREACHC(itplugin, listdbplugins) {
            if( !(*itplugin)->GetInfo(info) ) {
                RAVELOG_WARN(str(boost::format("failed to get plugin info: %s\n")%(*itplugin)->GetName()));
            }
        }
    }

    virtual bool LoadPlugin(const std::string& pname) { return _pdatabase->AddPlugin(pname); }
    virtual void ReloadPlugins() { _pdatabase->ReloadPlugins(); }
    virtual bool HasInterface(InterfaceType type, const string& interfacename) { return _pdatabase->HasInterface(type,interfacename); }

    virtual InterfaceBasePtr CreateInterface(InterfaceType type,const std::string& pinterfacename)
    {
        switch(type) {
        case PT_KinBody: return CreateKinBody(pinterfacename);
        case PT_Robot: return CreateRobot(pinterfacename);
        default: return _pdatabase->Create(shared_from_this(),type,pinterfacename);
        }
        throw openrave_exception("Bad interface type");
    }

    virtual RobotBasePtr CreateRobot(const std::string& pname)
    {
        RobotBasePtr probot;
        if( pname.size() > 0 )
            probot = _pdatabase->CreateRobot(shared_from_this(), pname);
        else {
            probot.reset(new RobotBase(shared_from_this()));
            probot->__strxmlid = "RobotBase";
        }
        return probot;
    }

    virtual KinBodyPtr CreateKinBody(const std::string& pname)
    {
        KinBodyPtr pbody;
        if( pname.size() > 0 )
            pbody = _pdatabase->CreateKinBody(shared_from_this(), pname);
        else {
            pbody.reset(new KinBody(PT_KinBody,shared_from_this()));
            pbody->__strxmlid = "KinBody";
        }
        return pbody;
    }

    virtual TrajectoryBasePtr CreateTrajectory(int nDOF) {
        return TrajectoryBasePtr(new TrajectoryBase(shared_from_this(),nDOF));
    }

    virtual PlannerBasePtr CreatePlanner(const std::string& pname) { return _pdatabase->CreatePlanner(shared_from_this(),pname); }
    virtual SensorSystemBasePtr CreateSensorSystem(const std::string& pname) { return _pdatabase->CreateSensorSystem(shared_from_this(),pname); }
    virtual ControllerBasePtr CreateController(const std::string& pname) { return _pdatabase->CreateController(shared_from_this(),pname); }
    virtual ProblemInstancePtr CreateProblem(const std::string& pname) { return _pdatabase->CreateProblem(shared_from_this(),pname); }
    virtual IkSolverBasePtr CreateIkSolver(const std::string& pname) { return _pdatabase->CreateIkSolver(shared_from_this(),pname); }
    virtual PhysicsEngineBasePtr CreatePhysicsEngine(const std::string& pname) { return _pdatabase->CreatePhysicsEngine(shared_from_this(),pname); }
    virtual SensorBasePtr CreateSensor(const std::string& pname) { return _pdatabase->CreateSensor(shared_from_this(),pname); }
    virtual CollisionCheckerBasePtr CreateCollisionChecker(const std::string& pname) { return _pdatabase->CreateCollisionChecker(shared_from_this(),pname); }
    virtual RaveViewerBasePtr CreateViewer(const std::string& pname) { return _pdatabase->CreateViewer(shared_from_this(),pname); }

    virtual void OwnInterface(InterfaceBasePtr pinterface)
    {
        CHECK_INTERFACE(pinterface);
        RAVELOG_VERBOSEA(str(boost::format("environment owning interface: %s\n")%pinterface->GetXMLId()));
        _listOwnedObjects.push_back(pinterface);
    }
    virtual void DisownInterface(InterfaceBasePtr pinterface)
    {
        CHECK_INTERFACE(pinterface);
        _listOwnedObjects.remove(pinterface);
    }

    virtual EnvironmentBasePtr CloneSelf(int options)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        boost::shared_ptr<Environment> penv(new Environment(false));
        penv->Clone(boost::static_pointer_cast<Environment const>(shared_from_this()),options);
        return penv;
    }
    
    virtual int LoadProblem(ProblemInstancePtr prob, const std::string& cmdargs)
    {
        CHECK_INTERFACE(prob);
        int ret = prob->main(cmdargs);
        if( ret != 0 )
            RAVELOG_WARNA("Error %d with executing problem %s\n", ret,prob->GetXMLId().c_str());
        else {
            boost::mutex::scoped_lock lock(_mutexProblems);
            _listProblems.push_back(prob);
        }

        return ret;
    }

    bool RemoveProblem(ProblemInstancePtr prob)
    {
        CHECK_INTERFACE(prob);
        boost::mutex::scoped_lock lock(_mutexProblems);
        list<ProblemInstancePtr>::iterator itprob = find(_listProblems.begin(), _listProblems.end(), prob);
        if( itprob != _listProblems.end() ) {
            (*itprob)->Destroy();
            _listProblems.erase(itprob);
            return true;
        }

        return false;
    }

    boost::shared_ptr<void> GetLoadedProblems(std::list<ProblemInstancePtr>& listProblems) const
    {
        boost::shared_ptr<boost::mutex::scoped_lock> plock(new boost::mutex::scoped_lock(_mutexProblems));
        listProblems = _listProblems;
        return plock;
    }

    virtual bool Load(const std::string& filename)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        bool bSuccess;
        if( _IsColladaFile(filename) ) {
            bSuccess = RaveParseColladaFile(shared_from_this(), filename);
        }
        else {
            bSuccess = ParseXMLFile(boost::shared_ptr<OpenRAVEXMLParser::EnvironmentXMLReader>(new OpenRAVEXMLParser::EnvironmentXMLReader(shared_from_this(),std::list<std::pair<std::string,std::string> >())), filename);
        }

        if( !bSuccess ) {
            RAVELOG_WARNA("load failed on file %s\n", filename.c_str());
        }
    
        return bSuccess;
    }

    virtual bool LoadXMLData(const std::string& data)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        if( !ParseXMLData(boost::shared_ptr<OpenRAVEXMLParser::EnvironmentXMLReader>(new OpenRAVEXMLParser::EnvironmentXMLReader(shared_from_this(),std::list<std::pair<std::string,std::string> >())),data) ) {
            RAVELOG_WARNA("load failed environment data\n");
            return false;
        }
        return true;
    }
    
    virtual bool Save(const std::string& filename)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        return RaveWriteColladaFile(shared_from_this(),filename);
    }

    virtual bool AddKinBody(KinBodyPtr pbody, bool bAnonymous)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_INTERFACE(pbody);
        if( !IsValidName(pbody->GetName()) ) {
            throw openrave_exception(str(boost::format("body name: \"%s\" is not valid")%pbody->GetName()));
        }
        if( !_CheckUniqueName(KinBodyConstPtr(pbody),!bAnonymous) ) {
            // continue to add random numbers until a unique name is found
            string newname;
            for(int i = 0;;++i) {
                newname = str(boost::format("%s%d")%pbody->GetName()%i);
                if( IsValidName(newname) )
                    break;
            }
            pbody->SetName(newname);
        }
        {
            boost::mutex::scoped_lock lock(_mutexBodies);
            _vecbodies.push_back(pbody);
            SetEnvironmentId(pbody);
            _nBodiesModifiedStamp++;
        }
        _pCurrentChecker->InitKinBody(pbody);
        _pPhysicsEngine->InitKinBody(pbody);
        pbody->_ComputeInternalInformation();
        return true;
    }
    virtual bool AddRobot(RobotBasePtr robot, bool bAnonymous)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_INTERFACE(robot);
        if( !IsValidName(robot->GetName()) ) {
            throw openrave_exception(str(boost::format("body name: \"%s\" is not valid")%robot->GetName()));
        }

        if( !_CheckUniqueName(KinBodyConstPtr(robot),!bAnonymous) ) {
            // continue to add random numbers until a unique name is found
            string newname;
            for(int i = 0;;++i) {
                newname = str(boost::format("%s%d")%robot->GetName()%i);
                if( IsValidName(newname) )
                    break;
            }
            robot->SetName(newname);
        }
        {
            boost::mutex::scoped_lock lock(_mutexBodies);
            _vecbodies.push_back(robot);
            _vecrobots.push_back(robot);
            SetEnvironmentId(robot);
            _nBodiesModifiedStamp++;
        }
        _pCurrentChecker->InitKinBody(robot);
        _pPhysicsEngine->InitKinBody(robot);
        robot->_ComputeInternalInformation();
        return true;
    }

    virtual bool RemoveKinBody(KinBodyPtr pbody)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_INTERFACE(pbody);

        bool bSuccess = false;
        boost::mutex::scoped_lock lock(_mutexBodies);

        vector<KinBodyPtr>::iterator it = std::find(_vecbodies.begin(), _vecbodies.end(), pbody);
        if( it != _vecbodies.end() ) {
            // before deleting, make sure no robots are grabbing it!!
            FOREACH(itrobot, _vecrobots) {
                if( (*itrobot)->IsGrabbing(*it) ) {
                    RAVELOG_WARNA("destroy %s already grabbed by robot %s!\n", pbody->GetName().c_str(), (*itrobot)->GetName().c_str());
                    (*itrobot)->Release(pbody);
                }
            }

            if( (*it)->IsRobot() ) {
                vector<RobotBasePtr>::iterator itrobot = std::find(_vecrobots.begin(), _vecrobots.end(), RaveInterfaceCast<RobotBase>(pbody));
                if( itrobot != _vecrobots.end() )
                    _vecrobots.erase(itrobot);
            }

            (*it)->SetPhysicsData(boost::shared_ptr<void>());
            (*it)->SetCollisionData(boost::shared_ptr<void>());
            RemoveEnvironmentId(pbody);
            _vecbodies.erase(it);
            _nBodiesModifiedStamp++;

            bSuccess = true;
        }

        return bSuccess;
    }

    virtual KinBodyPtr GetKinBody(const std::string& pname)
    {
        boost::mutex::scoped_lock lock(_mutexBodies);
        FOREACHC(it, _vecbodies) {
            if((*it)->GetName()==pname)
                return *it;
        }
        RAVELOG_VERBOSE(str(boost::format("Environment::GetKinBody - Error: Unknown body %s\n")%pname));
        return KinBodyPtr();
    }

    virtual RobotBasePtr GetRobot(const std::string& pname)
    {
        boost::mutex::scoped_lock lock(_mutexBodies);
        FOREACHC(it, _vecrobots) {
            if((*it)->GetName()==pname)
                return *it;
        }
        RAVELOG_VERBOSE(str(boost::format("Environment::GetRobot - Error: Unknown body %s\n")%pname));
        return RobotBasePtr();
    }

    virtual bool SetPhysicsEngine(PhysicsEngineBasePtr pengine)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());

        if( !!_pPhysicsEngine )
            _pPhysicsEngine->DestroyEnvironment();
        _pPhysicsEngine = pengine;
        if( !_pPhysicsEngine ) {
            RAVELOG_DEBUGA("disabling physics\n");
            _pPhysicsEngine.reset(new DummyPhysicsEngine(shared_from_this()));
        }
        else
            RAVELOG_DEBUGA(str(boost::format("setting %s physics engine\n")%_pPhysicsEngine->GetXMLId()));
        _pPhysicsEngine->InitEnvironment();
        return true;
    }

    virtual PhysicsEngineBasePtr GetPhysicsEngine() const { return _pPhysicsEngine; }

    static void __erase_collision_iterator(boost::weak_ptr<Environment> pweak, std::list<CollisionCallbackFn>::iterator* pit)
    {
        if( !!pit ) {
            boost::shared_ptr<Environment> penv = pweak.lock();
            if( !!penv )
                penv->_listRegisteredCollisionCallbacks.erase(*pit);
            delete pit;
        }
    }

    virtual boost::shared_ptr<void> RegisterCollisionCallback(const CollisionCallbackFn& callback) {
        EnvironmentMutex::scoped_lock lock(GetMutex());
        boost::shared_ptr<Environment> penv = boost::static_pointer_cast<Environment>(shared_from_this());
        return boost::shared_ptr<void>(new std::list<CollisionCallbackFn>::iterator(_listRegisteredCollisionCallbacks.insert(_listRegisteredCollisionCallbacks.end(),callback)), boost::bind(Environment::__erase_collision_iterator,boost::weak_ptr<Environment>(penv),_1));
    }
    virtual bool HasRegisteredCollisionCallbacks() const
    {
        EnvironmentMutex::scoped_lock lock(GetMutex());
        return _listRegisteredCollisionCallbacks.size() > 0;
    }
    virtual void GetRegisteredCollisionCallbacks(std::list<CollisionCallbackFn>& listcallbacks) const {
        EnvironmentMutex::scoped_lock lock(GetMutex());
        listcallbacks = _listRegisteredCollisionCallbacks;
    }

    virtual bool SetCollisionChecker(CollisionCheckerBasePtr pchecker)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        if( _pCurrentChecker == pchecker )
            return true;
        
        if( !!_pCurrentChecker )
            _pCurrentChecker->DestroyEnvironment(); // delete all resources
        _pCurrentChecker = pchecker;
        if( !_pCurrentChecker ) {
            RAVELOG_DEBUGA("disabling collisions\n");
            _pCurrentChecker.reset(new DummyCollisionChecker(shared_from_this()));
        }
        else
            RAVELOG_DEBUGA(str(boost::format("setting %s collision checker\n")%_pCurrentChecker->GetXMLId()));
        return _pCurrentChecker->InitEnvironment();
    }

    virtual CollisionCheckerBasePtr GetCollisionChecker() const { return _pCurrentChecker; }

    virtual bool CheckCollision(KinBodyConstPtr pbody1, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(pbody1);
        return _pCurrentChecker->CheckCollision(pbody1,report);
    }

    virtual bool CheckCollision(KinBodyConstPtr pbody1, KinBodyConstPtr pbody2, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(pbody1);
        CHECK_COLLISION_BODY(pbody2);
        return _pCurrentChecker->CheckCollision(pbody1,pbody2,report);
    }

    virtual bool CheckCollision(KinBody::LinkConstPtr plink, CollisionReportPtr report )
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(plink->GetParent());
        return _pCurrentChecker->CheckCollision(plink,report);
    }

    virtual bool CheckCollision(KinBody::LinkConstPtr plink1, KinBody::LinkConstPtr plink2, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(plink1->GetParent());
        CHECK_COLLISION_BODY(plink2->GetParent());
        return _pCurrentChecker->CheckCollision(plink1,plink2,report);
    }

    virtual bool CheckCollision(KinBody::LinkConstPtr plink, KinBodyConstPtr pbody, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(plink->GetParent());
        CHECK_COLLISION_BODY(pbody);
        return _pCurrentChecker->CheckCollision(plink,pbody,report);
    }
    
    virtual bool CheckCollision(KinBody::LinkConstPtr plink, const std::vector<KinBodyConstPtr>& vbodyexcluded, const std::vector<KinBody::LinkConstPtr>& vlinkexcluded, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(plink->GetParent());
        return _pCurrentChecker->CheckCollision(plink,vbodyexcluded,vlinkexcluded,report);
    }

    virtual bool CheckCollision(KinBodyConstPtr pbody, const std::vector<KinBodyConstPtr>& vbodyexcluded, const std::vector<KinBody::LinkConstPtr>& vlinkexcluded, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(pbody);
        return _pCurrentChecker->CheckCollision(pbody,vbodyexcluded,vlinkexcluded,report);
    }
    
    virtual bool CheckCollision(const RAY& ray, KinBody::LinkConstPtr plink, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(plink->GetParent());
        return _pCurrentChecker->CheckCollision(ray,plink,report);
    }
    virtual bool CheckCollision(const RAY& ray, KinBodyConstPtr pbody, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(pbody);
        return _pCurrentChecker->CheckCollision(ray,pbody,report);
    }
    virtual bool CheckCollision(const RAY& ray, CollisionReportPtr report)
    {
        return _pCurrentChecker->CheckCollision(ray,report);
    }

    virtual bool CheckSelfCollision(KinBodyConstPtr pbody, CollisionReportPtr report)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        CHECK_COLLISION_BODY(pbody);
        return _pCurrentChecker->CheckSelfCollision(pbody,report);
    }

    virtual void StepSimulation(dReal fTimeStep)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());

        uint64_t step = (uint64_t)ceil(1000000.0 * (double)fTimeStep);
        fTimeStep = (dReal)((double)step * 0.000001);

        // call the physics first to get forces
        _pPhysicsEngine->SimulateStep(fTimeStep);

        vector<KinBodyPtr> vecbodies;
        int nbodiesstamp;

        // make a copy instead of locking the mutex pointer since will be calling into user functions
        {
            boost::mutex::scoped_lock lock(_mutexBodies);
            vecbodies = _vecbodies; 
            nbodiesstamp = _nBodiesModifiedStamp;
        }
    
        FOREACH(it, vecbodies) {
            if( nbodiesstamp != _nBodiesModifiedStamp ) {
                // changed so have to check if current pointer is still active
                boost::mutex::scoped_lock lock(_mutexBodies);
                if( std::find(_vecbodies.begin(), _vecbodies.end(), *it) == _vecbodies.end() )
                    continue;
            }
            (*it)->SimulationStep(fTimeStep);
        }

        {
            boost::mutex::scoped_lock lock(_mutexProblems);
            FOREACH(itprob, _listProblems)
                (*itprob)->SimulationStep(fTimeStep);
        }

        _nCurSimTime += step;
    }

    virtual EnvironmentMutex& GetMutex() const { return _mutexEnvironment; }

    virtual void GetBodies(std::vector<KinBodyPtr>& bodies) const
    {
        boost::mutex::scoped_lock lock(_mutexBodies);
        bodies = _vecbodies;
    }

    virtual void GetRobots(std::vector<RobotBasePtr>& robots) const
    {
        boost::shared_ptr<boost::mutex::scoped_lock> plock(new boost::mutex::scoped_lock(_mutexBodies));
        robots = _vecrobots;
    }
    
    /// triangulation of the body including its current transformation. trimesh will be appended the new data.
    virtual bool Triangulate(KinBody::Link::TRIMESH& trimesh, KinBodyConstPtr pbody)
    {
        if( !pbody )
            return false;

        FOREACHC(it, pbody->GetLinks())
            trimesh.Append((*it)->GetCollisionData(), (*it)->GetTransform());
    
        return true;
    }

    /// general triangulation of the whole scene. trimesh will be appended the new data.
    virtual bool TriangulateScene(KinBody::Link::TRIMESH& trimesh, TriangulateOptions opts,const std::string& name)
    {
        boost::mutex::scoped_lock lock(_mutexBodies);
        FOREACH(itbody, _vecbodies) {
        
            RobotBasePtr robot;
            if( (*itbody)->IsRobot() )
                robot = RaveInterfaceCast<RobotBase>(*itbody);
        
            switch(opts) {
            case TO_Obstacles:
                if( !robot ) {
                    Triangulate(trimesh, (*itbody));
                }
                break;

            case TO_Robots:
                if( !!robot ) {
                    Triangulate(trimesh, (*itbody));
                }
                break;

            case TO_Everything:
                Triangulate(trimesh, (*itbody));
                break;

            case TO_Body:
                if( (*itbody)->GetName() == name ) {
                    Triangulate(trimesh, (*itbody));
                }
                break;

            case TO_AllExceptBody:
                if( (*itbody)->GetName() == name ) {
                    Triangulate(trimesh, (*itbody));
                }
                break;
            }
        }

        return true;
    }
    virtual bool TriangulateScene(KinBody::Link::TRIMESH& trimesh, TriangulateOptions opts)
    {
        return TriangulateScene(trimesh,opts,"");
    }

    virtual const std::string& GetHomeDirectory() const { return _homedirectory; }

    virtual RobotBasePtr ReadRobotXMLFile(const std::string& filename)
    {
        try {
            if( _IsColladaFile(filename) ) {
                RobotBasePtr robot;
                if( !RaveParseColladaFile(shared_from_this(), robot, filename) )
                    return RobotBasePtr();
                return robot;
            }
            else {
                InterfaceBasePtr pinterface = ReadInterfaceXMLFile(filename);
                BOOST_ASSERT(pinterface->GetInterfaceType() == PT_Robot );
                return RaveInterfaceCast<RobotBase>(pinterface);
            }
        }
        catch(const openrave_exception& ex) {
            RAVELOG_ERROR(str(boost::format("ReadRobotXMLFile exception: %s\n")%ex.what()));
        }
        return RobotBasePtr();
    }

    virtual RobotBasePtr ReadRobotXMLFile(RobotBasePtr robot, const std::string& filename, const std::list<std::pair<std::string,std::string> >& atts)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());

        if( !!robot ) {
            robot->SetGuiData(boost::shared_ptr<void>());
            boost::mutex::scoped_lock lock(_mutexBodies);
            if( std::find(_vecrobots.begin(),_vecrobots.end(),robot) != _vecrobots.end() )
                throw openrave_exception(str(boost::format("KinRobot::Init for %s, cannot Init a robot while it is added to the environment\n")%robot->GetName()));
        }

        if( _IsColladaFile(filename) ) {
            if( !RaveParseColladaFile(shared_from_this(), robot, filename) )
                return RobotBasePtr();
        }
        else {
            InterfaceBasePtr pinterface = robot;
            OpenRAVEXMLParser::InterfaceXMLReaderPtr preader = OpenRAVEXMLParser::CreateInterfaceReader(shared_from_this(), PT_Robot, pinterface, "robot", atts);
            bool bSuccess = ParseXMLFile(preader, filename);
            robot = RaveInterfaceCast<RobotBase>(pinterface);
            if( !bSuccess || !robot )
                return RobotBasePtr();
            robot->__strxmlfilename = filename;
        }
    
        return robot;
    }

    virtual RobotBasePtr ReadRobotXMLData(RobotBasePtr robot, const std::string& data, const std::list<std::pair<std::string,std::string> >& atts)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());

        if( !!robot ) {
            robot->SetGuiData(boost::shared_ptr<void>());
            boost::mutex::scoped_lock lock(_mutexBodies);
            if( std::find(_vecrobots.begin(),_vecrobots.end(),robot) != _vecrobots.end() )
                throw openrave_exception(str(boost::format("KinRobot::Init for %s, cannot Init a robot while it is added to the environment\n")%robot->GetName()));
        }

        // check for collada?
        InterfaceBasePtr pinterface = robot;
        OpenRAVEXMLParser::InterfaceXMLReaderPtr preader = OpenRAVEXMLParser::CreateInterfaceReader(shared_from_this(), PT_Robot, pinterface, "robot", atts);
        bool bSuccess = ParseXMLData(preader, data);
        robot = RaveInterfaceCast<RobotBase>(pinterface);
        if( !bSuccess || !robot )
            return RobotBasePtr();
        robot->__strxmlfilename = preader->_filename;
        return robot;
    }

    virtual KinBodyPtr ReadKinBodyXMLFile(const std::string& filename)
    {
        try {
            if( _IsColladaFile(filename) ) {
                KinBodyPtr body;
                if( !RaveParseColladaFile(shared_from_this(), body, filename) )
                    return KinBodyPtr();
                return body;
            }
            else {
                InterfaceBasePtr pinterface = ReadInterfaceXMLFile(filename);
                BOOST_ASSERT(pinterface->GetInterfaceType() == PT_KinBody );
                return RaveInterfaceCast<KinBody>(pinterface);
            }
        }
        catch(const openrave_exception& ex) {
            RAVELOG_ERROR(str(boost::format("ReadRobotXMLFile exception: %s\n")%ex.what()));
        }
        return KinBodyPtr();
    }

    virtual KinBodyPtr ReadKinBodyXMLFile(KinBodyPtr body, const std::string& filename, const std::list<std::pair<std::string,std::string> >& atts)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());

        if( !!body ) {
            body->SetGuiData(boost::shared_ptr<void>());
            boost::mutex::scoped_lock lock(_mutexBodies);
            if( std::find(_vecbodies.begin(),_vecbodies.end(),body) != _vecbodies.end() )
                throw openrave_exception(str(boost::format("KinBody::Init for %s, cannot Init a body while it is added to the environment\n")%body->GetName()));
        }

        if( _IsColladaFile(filename) ) {
            if( !RaveParseColladaFile(shared_from_this(), body, filename) )
                return KinBodyPtr();
        }
        else {
            InterfaceBasePtr pinterface = body;
            OpenRAVEXMLParser::InterfaceXMLReaderPtr preader = OpenRAVEXMLParser::CreateInterfaceReader(shared_from_this(), PT_KinBody, pinterface, "kinbody", atts);
            bool bSuccess = ParseXMLFile(preader, filename);
            body = RaveInterfaceCast<KinBody>(pinterface);
            if( !bSuccess || !body )
                return KinBodyPtr();
            body->__strxmlfilename = filename;
        }

        return body;
    }

    virtual KinBodyPtr ReadKinBodyXMLData(KinBodyPtr body, const std::string& data, const std::list<std::pair<std::string,std::string> >& atts)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());

        if( !!body ) {
            body->SetGuiData(boost::shared_ptr<void>());
            boost::mutex::scoped_lock lock(_mutexBodies);
            if( std::find(_vecbodies.begin(),_vecbodies.end(),body) != _vecbodies.end() )
                throw openrave_exception(str(boost::format("KinBody::Init for %s, cannot Init a body while it is added to the environment\n")%body->GetName()));
        }

        // check for collada?
        InterfaceBasePtr pinterface = body;
        OpenRAVEXMLParser::InterfaceXMLReaderPtr preader = OpenRAVEXMLParser::CreateInterfaceReader(shared_from_this(), PT_KinBody, pinterface, "kinbody", atts);
        bool bSuccess = ParseXMLData(preader, data);
        body = RaveInterfaceCast<KinBody>(pinterface);
        if( !bSuccess || !body )
            return KinBodyPtr();
        body->__strxmlfilename = preader->_filename;
        return body;
    }

    virtual InterfaceBasePtr ReadInterfaceXMLFile(const std::string& filename)
    {
        try {
            EnvironmentMutex::scoped_lock lockenv(GetMutex());
            OpenRAVEXMLParser::SetDataDirs(GetDataDirs());
            boost::shared_ptr<OpenRAVEXMLParser::GlobalInterfaceXMLReader> preader(new OpenRAVEXMLParser::GlobalInterfaceXMLReader(shared_from_this()));
            bool bSuccess = RaveParseXMLFile(preader, filename);
            if( !bSuccess || !preader->GetInterface())
                return InterfaceBasePtr();
            preader->GetInterface()->__strxmlfilename = filename;
            return preader->GetInterface();
        }
        catch(const openrave_exception& ex) {
            RAVELOG_ERROR(str(boost::format("ReadInterfaceXMLFile exception: %s\n")%ex.what()));
        }
        return InterfaceBasePtr();
    }

    virtual InterfaceBasePtr ReadInterfaceXMLFile(InterfaceBasePtr pinterface, InterfaceType type, const std::string& filename, const std::list<std::pair<std::string,std::string> >& atts)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        if( (type == PT_KinBody || type == PT_Robot) && _IsColladaFile(filename) ) {
            if( type == PT_KinBody ) {
                BOOST_ASSERT(!pinterface|| (pinterface->GetInterfaceType()==PT_KinBody||pinterface->GetInterfaceType()==PT_Robot));
                KinBodyPtr pbody = RaveInterfaceCast<KinBody>(pinterface);
                if( !RaveParseColladaFile(shared_from_this(), pbody, filename) )
                    return InterfaceBasePtr();
                pinterface = pbody;
            }
            else if( type == PT_Robot ) {
                BOOST_ASSERT(!pinterface||pinterface->GetInterfaceType()==PT_Robot);
                RobotBasePtr probot = RaveInterfaceCast<RobotBase>(pinterface);
                if( !RaveParseColladaFile(shared_from_this(), probot, filename) )
                    return InterfaceBasePtr();
                pinterface = probot;
            }
            else
                return InterfaceBasePtr();
            pinterface->__strxmlfilename = filename;
        }
        else {
            OpenRAVEXMLParser::InterfaceXMLReaderPtr preader = OpenRAVEXMLParser::CreateInterfaceReader(shared_from_this(), type, pinterface, RaveGetInterfaceName(type), atts);
            bool bSuccess = ParseXMLFile(preader, filename);
            if( !bSuccess )
                return InterfaceBasePtr();
            pinterface->__strxmlfilename = filename;
        }
        return pinterface;
    }

    virtual InterfaceBasePtr ReadInterfaceXMLData(InterfaceBasePtr pinterface, InterfaceType type, const std::string& data, const std::list<std::pair<std::string,std::string> >& atts)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());

        // check for collada?
        OpenRAVEXMLParser::InterfaceXMLReaderPtr preader = OpenRAVEXMLParser::CreateInterfaceReader(shared_from_this(), type, pinterface, RaveGetInterfaceName(type), atts);
        bool bSuccess = ParseXMLData(preader, data);
        if( !bSuccess )
            return InterfaceBasePtr();
        pinterface->__strxmlfilename = preader->_filename;
        return pinterface;
    }

    virtual boost::shared_ptr<void> RegisterXMLReader(InterfaceType type, const std::string& xmltag, const CreateXMLReaderFn& fn)
    {
        CreateXMLReaderFn oldfn = OpenRAVEXMLParser::RegisterXMLReader(type,xmltag,fn);
        return boost::shared_ptr<void>((void*)1, boost::bind(&OpenRAVEXMLParser::UnregisterXMLReader,type,xmltag,oldfn));
    }
    
    virtual bool ParseXMLFile(BaseXMLReaderPtr preader, const std::string& filename)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        OpenRAVEXMLParser::SetDataDirs(GetDataDirs());
        return OpenRAVEXMLParser::RaveParseXMLFile(preader, filename);
    }

    virtual bool ParseXMLData(BaseXMLReaderPtr preader, const std::string& pdata)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        OpenRAVEXMLParser::SetDataDirs(GetDataDirs());
        return OpenRAVEXMLParser::RaveParseXMLData(preader, pdata);
    }

    virtual bool AttachViewer(RaveViewerBasePtr pnewviewer)
    {
        if( _pCurrentViewer == pnewviewer )
            return true;

        if( !!_pCurrentViewer )
            _pCurrentViewer->quitmainloop();
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        
        _pCurrentViewer = pnewviewer;
        if( !_pCurrentViewer )
            _pCurrentViewer.reset(new DummyRaveViewer(shared_from_this()));

        if( _pCurrentViewer->GetEnv() != shared_from_this() )
            throw openrave_exception("Viewer needs to be created by the current environment");
        return true;
    }

    virtual RaveViewerBasePtr GetViewer() const
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        return _pCurrentViewer;
    }

    virtual GraphHandlePtr plot3(const float* ppoints, int numPoints, int stride, float fPointSize, const RaveVector<float>& color, int drawstyle)
    {
        return GraphHandlePtr(_pCurrentViewer->plot3(ppoints, numPoints, stride, fPointSize, color, drawstyle), GRAPH_DELETER);
    }
    virtual GraphHandlePtr plot3(const float* ppoints, int numPoints, int stride, float fPointSize, const float* colors, int drawstyle, bool bhasalpha)
    {
        return GraphHandlePtr(_pCurrentViewer->plot3(ppoints, numPoints, stride, fPointSize, colors, drawstyle, bhasalpha), GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawlinestrip(const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color)
    {
        return GraphHandlePtr(_pCurrentViewer->drawlinestrip(ppoints, numPoints, stride, fwidth,color), GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawlinestrip(const float* ppoints, int numPoints, int stride, float fwidth, const float* colors)
    {
        return GraphHandlePtr(_pCurrentViewer->drawlinestrip(ppoints, numPoints, stride, fwidth,colors),GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawlinelist(const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color)
    {
        return GraphHandlePtr(_pCurrentViewer->drawlinelist(ppoints, numPoints, stride, fwidth,color), GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawlinelist(const float* ppoints, int numPoints, int stride, float fwidth, const float* colors)
    {
        return GraphHandlePtr(_pCurrentViewer->drawlinelist(ppoints, numPoints, stride, fwidth,colors), GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawarrow(const RaveVector<float>& p1, const RaveVector<float>& p2, float fwidth, const RaveVector<float>& color)
    {
        return GraphHandlePtr(_pCurrentViewer->drawarrow(p1,p2,fwidth,color),GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawbox(const RaveVector<float>& vpos, const RaveVector<float>& vextents)
    {
        return GraphHandlePtr(_pCurrentViewer->drawbox(vpos, vextents), GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawplane(const RaveTransform<float>& tplane, const RaveVector<float>& vextents, const boost::multi_array<float,3>& vtexture)
    {
        return GraphHandlePtr(_pCurrentViewer->drawplane(tplane, vextents, vtexture), GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawtrimesh(const float* ppoints, int stride, const int* pIndices, int numTriangles, const RaveVector<float>& color)
    {
        return GraphHandlePtr(_pCurrentViewer->drawtrimesh(ppoints, stride, pIndices, numTriangles, color), GRAPH_DELETER);
    }
    virtual GraphHandlePtr drawtrimesh(const float* ppoints, int stride, const int* pIndices, int numTriangles, const boost::multi_array<float,2>& colors)
    {
        return GraphHandlePtr(_pCurrentViewer->drawtrimesh(ppoints, stride, pIndices, numTriangles, colors), GRAPH_DELETER);
    }
    virtual void _CloseGraphCallback(RaveViewerBaseWeakPtr wviewer, void* handle)
    {
        RaveViewerBasePtr viewer = wviewer.lock();
        if( !!viewer )
            viewer->closegraph(handle);
    }

    virtual KinBodyPtr GetBodyFromNetworkId(int id)
    {
        RAVELOG_INFO("GetBodyFromNetworkId is deprecated, use GetBodyFromEnvironmentId instead\n");
        return GetBodyFromEnvironmentId(id);
    }

    virtual KinBodyPtr GetBodyFromEnvironmentId(int id)
    {
        boost::mutex::scoped_lock lock(_mutexBodies);
        boost::mutex::scoped_lock locknetwork(_mutexEnvironmentIds);
        map<int, KinBodyWeakPtr>::iterator it = _mapBodies.find(id);
        if( it != _mapBodies.end() )
            return KinBodyPtr(it->second);
        return KinBodyPtr();
    }

    virtual void StartSimulation(dReal fDeltaTime, bool bRealTime)
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        _bEnableSimulation = true;
        _fDeltaSimTime = fDeltaTime;
        _bRealTime = bRealTime;
        _nCurSimTime = 0;
        _nSimStartTime = GetMicroTime();
    }

    virtual bool IsSimulationRunning() const { return _bEnableSimulation; }

    virtual void StopSimulation()
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        _bEnableSimulation = false;
        _fDeltaSimTime = 1.0f;
    }
    virtual uint64_t GetSimulationTime() { return _nCurSimTime; }

    virtual void SetDebugLevel(DebugLevel level) { RaveSetDebugLevel(level); }
    virtual DebugLevel GetDebugLevel() const { return RaveGetDebugLevel(); }

    virtual void GetPublishedBodies(std::vector<KinBody::BodyState>& vbodies)
    {
        boost::mutex::scoped_lock lock(_mutexBodies);
        vbodies = _vPublishedBodies;
    }

    virtual void UpdatePublishedBodies()
    {
        EnvironmentMutex::scoped_lock lockenv(GetMutex());
        boost::mutex::scoped_lock lock(_mutexBodies);

        // updated the published bodies
        _vPublishedBodies.resize(_vecbodies.size());
            
        vector<KinBody::BodyState>::iterator itstate = _vPublishedBodies.begin();
        FOREACH(itbody, _vecbodies) {
            itstate->pbody = *itbody;
            (*itbody)->GetBodyTransformations(itstate->vectrans);
            (*itbody)->GetDOFValues(itstate->jointvalues);
            itstate->strname =(*itbody)->GetName();
            itstate->pguidata = (*itbody)->GetGuiData();
            itstate->environmentid = (*itbody)->GetEnvironmentId();
            ++itstate;
        }
    }

    const vector<string>& GetDataDirs() const { return _vdatadirs; }

protected:

    virtual void Clone(boost::shared_ptr<Environment const> r, int options)
    {
        Destroy();
        AttachViewer(RaveViewerBasePtr());
        SetCollisionChecker(CollisionCheckerBasePtr());
        SetPhysicsEngine(PhysicsEngineBasePtr());

        _nBodiesModifiedStamp = r->_nBodiesModifiedStamp;
        _homedirectory = r->_homedirectory;
        _pCurrentViewer.reset(new DummyRaveViewer(shared_from_this()));
        _fDeltaSimTime = r->_fDeltaSimTime;
        _nCurSimTime = 0;
        _nSimStartTime = GetMicroTime();
        _nEnvironmentIndex = r->_nEnvironmentIndex;
        _bRealTime = r->_bRealTime;

        _bDestroying = false;
        _bDestroyed = false;
        _bEnableSimulation = r->_bEnableSimulation;

        SetDebugLevel(r->GetDebugLevel());

        // a little tricky due to a deadlocking situation
        std::map<int, KinBodyWeakPtr> mapBodies;
        {
            boost::mutex::scoped_lock locknetworkid(_mutexEnvironmentIds);
            mapBodies = _mapBodies;
            _mapBodies.clear();
        }
        mapBodies.clear();

        _pdatabase = r->_pdatabase;

        _vplugindirs = r->_vplugindirs;
        _vdatadirs = r->_vdatadirs;

        EnvironmentMutex::scoped_lock lock(GetMutex());
        boost::mutex::scoped_lock locknetworkid(_mutexEnvironmentIds);

        // clone collision and physics
        if( !!r->GetCollisionChecker() ) {
            SetCollisionChecker(CreateCollisionChecker(r->GetCollisionChecker()->GetXMLId()));
        }

        if( options & Clone_Bodies ) {
            boost::mutex::scoped_lock lock(r->_mutexBodies);
            FOREACHC(itrobot, r->_vecrobots) {
                RobotBasePtr pnewrobot = _pdatabase->CreateRobot(shared_from_this(), (*itrobot)->GetXMLId());
                if( !pnewrobot ) {
                    RAVELOG_ERRORA("failed to create robot %s\n", (*itrobot)->GetXMLId().c_str());
                    continue;
                }

                if( !pnewrobot->Clone(*itrobot, options)) {
                    RAVELOG_ERRORA("failed to clone robot %s\n", (*itrobot)->GetName().c_str());
                    continue;
                }

                pnewrobot->_environmentid = (*itrobot)->GetEnvironmentId();

                // note that pointers will not be correct
                pnewrobot->_vGrabbedBodies = (*itrobot)->_vGrabbedBodies;
                pnewrobot->_listAttachedBodies = (*itrobot)->_listAttachedBodies;

                BOOST_ASSERT( _mapBodies.find(pnewrobot->GetEnvironmentId()) == _mapBodies.end() );
                _mapBodies[pnewrobot->GetEnvironmentId()] = pnewrobot;
                _vecbodies.push_back(pnewrobot);
                _vecrobots.push_back(pnewrobot);
            }
            FOREACHC(itbody, r->_vecbodies) {
                if( _mapBodies.find((*itbody)->GetEnvironmentId()) != _mapBodies.end() )
                    continue;
                KinBodyPtr pnewbody(new KinBody(PT_KinBody,shared_from_this()));
                if( !pnewbody->Clone(*itbody,options) ) {
                    RAVELOG_ERRORA("failed to clone body %s\n", (*itbody)->GetName().c_str());
                    continue;
                }

                pnewbody->_environmentid = (*itbody)->GetEnvironmentId();
                
                // note that pointers will not be correct
                pnewbody->_listAttachedBodies = (*itbody)->_listAttachedBodies;

                // note that pointers will not be correct
                _mapBodies[pnewbody->GetEnvironmentId()] = pnewbody;
                _vecbodies.push_back(pnewbody);
            }

            // process attached bodies
            FOREACH(itbody, _vecbodies) {
                list<KinBodyWeakPtr> listnew;
                FOREACH(itatt, (*itbody)->_listAttachedBodies) {
                    KinBodyPtr patt = itatt->lock();
                    if( !!patt ) {
                        BOOST_ASSERT( _mapBodies.find(patt->GetEnvironmentId()) != _mapBodies.end() );
                        listnew.push_back(_mapBodies[patt->GetEnvironmentId()]);
                    }
                }
                (*itbody)->_listAttachedBodies = listnew;
            }

            // process grabbed bodies
            FOREACH(itrobot, _vecrobots) {
                FOREACH(itgrab, (*itrobot)->_vGrabbedBodies) {
                    KinBodyPtr pbody(itgrab->pbody);
                    BOOST_ASSERT( !!pbody && _mapBodies.find(pbody->GetEnvironmentId()) != _mapBodies.end());
                    itgrab->pbody = _mapBodies[pbody->GetEnvironmentId()];
                    itgrab->plinkrobot = (*itrobot)->GetLinks().at(KinBody::LinkPtr(itgrab->plinkrobot)->GetIndex());

                    vector<KinBody::LinkConstPtr> vnew;
                    FOREACH(itlink, itgrab->vCollidingLinks)
                        vnew.push_back((*itrobot)->_veclinks.at((*itlink)->GetIndex()));
                    itgrab->vCollidingLinks = vnew;
                    vnew.resize(0);
                    FOREACH(itlink, itgrab->vNonCollidingLinks)
                        vnew.push_back((*itrobot)->_veclinks.at((*itlink)->GetIndex()));
                    itgrab->vNonCollidingLinks = vnew;
                }
            }

            FOREACH(itbody, _vecbodies) {
                GetCollisionChecker()->InitKinBody(*itbody);
                GetPhysicsEngine()->InitKinBody(*itbody);
            }
            FOREACH(itbody,_vecbodies)
                (*itbody)->_ComputeInternalInformation();
        }
        if( options & Clone_Viewer ) {
            if( !!r->GetViewer() ) {
                AttachViewer(CreateViewer(r->GetViewer()->GetXMLId()));
            }
        }
    
        if( options & Clone_Simulation ) {
            if( !!r->GetPhysicsEngine() ) {
                SetPhysicsEngine(CreatePhysicsEngine(r->GetPhysicsEngine()->GetXMLId()));
            }
        
            _bEnableSimulation = r->_bEnableSimulation;
            _nCurSimTime = r->_nCurSimTime;
            _nSimStartTime = r->_nSimStartTime;
        }

        if( !!_threadSimulation )
            _threadSimulation->join();
        _threadSimulation.reset(new boost::thread(boost::bind(&Environment::_SimulationThread,this)));
    }

    virtual bool _CheckUniqueName(KinBodyConstPtr pbody, bool bDoThrow=false) const
    {
        FOREACHC(itbody,_vecbodies) {
            if( *itbody != pbody && (*itbody)->GetName() == pbody->GetName() ) {
                if( bDoThrow ) {
                    throw openrave_exception(str(boost::format("body %s does not have unique name")%pbody->GetName()));
                }
                return false;
            }
        }
        return true;
    }

    class DummyPhysicsEngine : public PhysicsEngineBase
    {
    public:
        DummyPhysicsEngine(EnvironmentBasePtr penv) : PhysicsEngineBase(penv) {}
        virtual bool SetPhysicsOptions(int physicsoptions) { return true; }
        virtual int GetPhysicsOptions() const { return 0; }
        
        virtual bool SetPhysicsOptions(std::ostream& sout, std::istream& sinput) { return true; }

        virtual bool InitEnvironment() { return true; }
        virtual void DestroyEnvironment() {}
        
        virtual bool InitKinBody(KinBodyPtr pbody) { SetPhysicsData(pbody, boost::shared_ptr<void>()); return true; } 
        virtual bool DestroyKinBody(KinBodyPtr pbody) { SetPhysicsData(pbody, boost::shared_ptr<void>()); return true; }

        virtual bool GetLinkVelocity(KinBody::LinkConstPtr plink, Vector& linearvel, Vector& angularvel) { return true; }
        virtual bool SetLinkVelocity(KinBody::LinkPtr plink, const Vector& linearvel, const Vector& angularvel) { return true; }
        virtual bool SetBodyVelocity(KinBodyPtr pbody, const Vector& linearvel, const Vector& angularvel, const std::vector<dReal>& pJointVelocity) { return true; }
        virtual bool SetBodyVelocity(KinBodyPtr pbody, const std::vector<Vector>& pLinearVelocities, const std::vector<Vector>& pAngularVelocities) { return true; }
        virtual bool GetBodyVelocity(KinBodyConstPtr pbody, Vector& linearvel, Vector& angularvel)
        {
            if( !pbody )
                return false;
            linearvel = Vector(0,0,0,0);
            angularvel = Vector(0,0,0,0);
            return true;
        }

        virtual bool GetBodyVelocity(KinBodyConstPtr pbody, Vector& linearvel, Vector& angularvel, std::vector<dReal>& pJointVelocity)
        {
            if( !pbody )
                return false;
            linearvel = Vector(0,0,0,0);
            angularvel = Vector(0,0,0,0);
    
            pJointVelocity.resize(0);
            pJointVelocity.resize(pbody->GetDOF(),0);
            return true;
        }

        virtual bool GetBodyVelocity(KinBodyConstPtr pbody, std::vector<Vector>& pLinearVelocities, std::vector<Vector>& pAngularVelocities)
        {
            pLinearVelocities.resize(0);
            pLinearVelocities.resize(3*pbody->GetLinks().size(),Vector());
            pAngularVelocities.resize(0);
            pAngularVelocities.resize(3*pbody->GetLinks().size(),Vector());
            return true;
        }

        virtual bool SetJointVelocity(KinBody::JointPtr pjoint, const std::vector<dReal>& pJointVelocity) { return true; }
        virtual bool GetJointVelocity(KinBody::JointConstPtr pjoint, std::vector<dReal>& pJointVelocity)
        {
            pJointVelocity.resize(0);
            pJointVelocity.resize(pjoint->GetDOF(),0);
            return true;
        }

        virtual bool SetBodyForce(KinBody::LinkPtr plink, const Vector& force, const Vector& position, bool bAdd) { return true; }
        virtual bool SetBodyTorque(KinBody::LinkPtr plink, const Vector& torque, bool bAdd) { return true; }
        virtual bool AddJointTorque(KinBody::JointPtr pjoint, const std::vector<dReal>& pTorques) { return true; }

        virtual void SetGravity(const Vector& gravity) {}
        virtual Vector GetGravity() { return Vector(0,0,0); }

        virtual bool GetLinkForceTorque(KinBody::LinkConstPtr plink, Vector& force, Vector& torque) {
            force = Vector(0,0,0);
            torque = Vector(0,0,0);
            return true;
        }

        virtual void SimulateStep(dReal fTimeElapsed) { }
    };

    class DummyCollisionChecker : public CollisionCheckerBase
    {
    public:
        DummyCollisionChecker(EnvironmentBasePtr penv) : CollisionCheckerBase(penv) {}
        virtual ~DummyCollisionChecker() {}

        virtual bool InitEnvironment() { return true; }
        virtual void DestroyEnvironment() {}

        virtual bool InitKinBody(KinBodyPtr pbody) { SetCollisionData(pbody, boost::shared_ptr<void>()); return true; }
        virtual bool DestroyKinBody(KinBodyPtr pbody) { SetCollisionData(pbody, boost::shared_ptr<void>()); return true; }
        virtual bool Enable(KinBodyConstPtr pbody, bool bEnable) { return true; }
        virtual bool EnableLink(KinBody::LinkConstPtr pbody, bool bEnable) { return true; }

        virtual bool SetCollisionOptions(int collisionoptions) { return true; }
        virtual int GetCollisionOptions() const { return 0; }

        virtual bool SetCollisionOptions(std::ostream& sout, std::istream& sinput) { return true; }

        virtual bool CheckCollision(KinBodyConstPtr pbody1, CollisionReportPtr) { return false; }
        virtual bool CheckCollision(KinBodyConstPtr pbody1, KinBodyConstPtr pbody2, CollisionReportPtr) { return false; }
        virtual bool CheckCollision(KinBody::LinkConstPtr plink, CollisionReportPtr) { return false; }
        virtual bool CheckCollision(KinBody::LinkConstPtr plink1, KinBody::LinkConstPtr plink2, CollisionReportPtr) { return false; }
        virtual bool CheckCollision(KinBody::LinkConstPtr plink, KinBodyConstPtr pbody, CollisionReportPtr) { return false; }
    
        virtual bool CheckCollision(KinBody::LinkConstPtr plink, const std::vector<KinBodyConstPtr>& vbodyexcluded, const std::vector<KinBody::LinkConstPtr>& vlinkexcluded, CollisionReportPtr) { return false; }
        virtual bool CheckCollision(KinBodyConstPtr pbody, const std::vector<KinBodyConstPtr>& vbodyexcluded, const std::vector<KinBody::LinkConstPtr>& vlinkexcluded, CollisionReportPtr) { return false; }
    
        virtual bool CheckCollision(const RAY& ray, KinBody::LinkConstPtr plink, CollisionReportPtr) { return false; }
        virtual bool CheckCollision(const RAY& ray, KinBodyConstPtr pbody, CollisionReportPtr) { return false; }
        virtual bool CheckCollision(const RAY& ray, CollisionReportPtr) { return false; }
        virtual bool CheckSelfCollision(KinBodyConstPtr pbody, CollisionReportPtr) { return false; }

        virtual void SetTolerance(dReal tolerance) {}
    };

    /// Base class for the graphics and gui engine. Derive a class called
    /// RaveViewer for anything more specific
    class DummyRaveViewer : public RaveViewerBase
    {
    public:
        DummyRaveViewer(EnvironmentBasePtr penv) : RaveViewerBase(penv) {
            _bQuitMainLoop = true;
        }
        virtual ~DummyRaveViewer() {}

        //void   AddItem(Item *pItem, bool bVisibility = false);
        /// reset the camera depending on its mode
        virtual void UpdateCameraTransform() {}

        /// goes into the main loop
        virtual int main(bool bShow) {
            _bQuitMainLoop = false;
            GetEnv()->StartSimulation(0.01f,true);

            while(!_bQuitMainLoop) {
                AdvanceFrame(true);
            }

            return 0;
        }
        
        virtual void quitmainloop() {
            if( !_bQuitMainLoop ) {
                GetEnv()->StopSimulation();
                _bQuitMainLoop = true;
            }
        }

        /// methods relating to playback
        virtual void AdvanceFrame(bool bForward) {
            // update the camera
            UpdateCameraTransform();

            if( _bSaveVideo )
                _RecordVideo();

            // process sensors (camera images)
            Sleep(1);
        }

        virtual boost::shared_ptr<void> LockGUI() { return boost::shared_ptr<void>(); }

        virtual void* plot3(const float* ppoints, int numPoints, int stride, float fPointSize, const RaveVector<float>& color, int drawstyle=0) { return NULL; }
        virtual void* plot3(const float* ppoints, int numPoints, int stride, float fPointSize, const float* colors, int drawstyle, bool bhasalpha) { return NULL; }

        virtual void* drawlinestrip(const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color) { return NULL; }
        virtual void* drawlinestrip(const float* ppoints, int numPoints, int stride, float fwidth, const float* colors) { return NULL; }

        virtual void* drawlinelist(const float* ppoints, int numPoints, int stride, float fwidth, const RaveVector<float>& color) { return NULL; }
        virtual void* drawlinelist(const float* ppoints, int numPoints, int stride, float fwidth, const float* colors) { return NULL; }

        virtual void* drawarrow(const RaveVector<float>& p1, const RaveVector<float>& p2, float fwidth, const RaveVector<float>& color) { return NULL; }

        virtual void* drawbox(const RaveVector<float>& vpos, const RaveVector<float>& vextents) { return NULL; }
        virtual void* drawplane(const RaveTransform<float>& tplane, const RaveVector<float>& vextents, const boost::multi_array<float,3>& vtexture) { return NULL; }

        virtual void* drawtrimesh(const float* ppoints, int stride, const int* pIndices, int numTriangles, const RaveVector<float>& color) { return NULL; }
        virtual void* drawtrimesh(const float* ppoints, int stride, const int* pIndices, int numTriangles, const boost::multi_array<float,2>& colors) { return NULL; }

        virtual void closegraph(void* handle) {}
        virtual void deselect() {}

        virtual void SetCamera(const RaveTransform<float>& trans) {}
        virtual void SetCameraLookAt(const RaveVector<float>& lookat, const RaveVector<float>& campos, const RaveVector<float>& camup) {}
        virtual RaveTransform<float> GetCameraTransform() { return RaveTransform<float>(); }
        
        virtual bool GetCameraImage(std::vector<uint8_t>& memory, int width, int height, const RaveTransform<float>& t, const SensorBase::CameraIntrinsics& KK) { return false; }
        
        virtual bool WriteCameraImage(int width, int height, const RaveTransform<float>& t, const SensorBase::CameraIntrinsics& KK, const std::string& filename, const std::string& extension) { return false; }
    
        virtual void Reset() {}
        virtual void SetBkgndColor(const RaveVector<float>& color) {}

        virtual bool _RecordVideo() { return false; }

        virtual boost::shared_ptr<void> RegisterCallback(int properties, const ViewerCallbackFn& fncallback) { return boost::shared_ptr<void>(); }
        virtual void SetEnvironmentSync(bool) {}
        virtual void EnvironmentSync() {}

        virtual void ViewerSetSize(int w, int h) {}
        virtual void ViewerMove(int x, int y) {}
        virtual void ViewerSetTitle(const std::string& ptitle) {};
        
        virtual bool LoadModel(const std::string& pfilename) { return true; }
    protected:

        bool _bTimeElapsed;
        bool _bSaveVideo;
    
        // environment thread related
        bool _bQuitMainLoop;
    };

    virtual void SetEnvironmentId(KinBodyPtr pbody)
    {
        boost::mutex::scoped_lock locknetworkid(_mutexEnvironmentIds);
        int id = _nEnvironmentIndex++;
        BOOST_ASSERT( _mapBodies.find(id) == _mapBodies.end() );
        pbody->_environmentid=id;
        _mapBodies[id] = pbody;
    }

    virtual void RemoveEnvironmentId(KinBodyPtr pbody)
    {
        boost::mutex::scoped_lock locknetworkid(_mutexEnvironmentIds);
        _mapBodies.erase(pbody->_environmentid);
        pbody->_environmentid = 0;
    }

    void _SimulationThread()
    {
        uint64_t nLastUpdateTime = GetMicroTime();
        uint64_t nLastSleptTime = GetMicroTime();
        while( !_bDestroying ) {
            bool bNeedSleep = true;

            if( _bEnableSimulation ) {
                bNeedSleep = false;
                EnvironmentMutex::scoped_lock lockenv(GetMutex());
                int64_t deltasimtime = (int64_t)(_fDeltaSimTime*1000000.0f);
                StepSimulation(_fDeltaSimTime);
                uint64_t passedtime = GetMicroTime()-_nSimStartTime;
                int64_t sleeptime = _nCurSimTime-passedtime;
                if( _bRealTime ) {
                    if( sleeptime > 2*deltasimtime && sleeptime > 2000 ) {
                        lockenv.unlock();
                        // sleep for less time since sleep isn't accurate at all and we have a 7ms buffer
                        Sleep( max((int)(deltasimtime + (sleeptime-2*deltasimtime)/2)/1000,1) );
                        //RAVELOG_INFO("sleeping %d(%d), slept: %d\n",(int)(_nCurSimTime-passedtime),(int)((sleeptime-(deltasimtime/2))/1000));
                        nLastSleptTime = GetMicroTime();
                    }
                    else if( sleeptime < -3*deltasimtime ) {
                        // simulation is getting late, so catch up
                        //RAVELOG_INFO("sim catching up: %d\n",-(int)sleeptime);
                        _nSimStartTime += -sleeptime;//deltasimtime;
                    }
                }
                
                //RAVELOG_INFOA("sim: %f, real: %f\n",_nCurSimTime*1e-6f,(GetMicroTime()-_nSimStartTime)*1e-6f);
            }

            if( GetMicroTime()-nLastSleptTime > 100000 ) {
                Sleep(1); bNeedSleep = false;
                nLastSleptTime = GetMicroTime();
            }

            if( GetMicroTime()-nLastUpdateTime > 10000 ) {
                EnvironmentMutex::scoped_lock lockenv(GetMutex());
                nLastUpdateTime = GetMicroTime();
                UpdatePublishedBodies();
            }

            if( bNeedSleep ) {
                Sleep(1);
                _pdatabase->CleanupUnusedLibraries();
            }
        }
    }

    static bool _IsColladaFile(const std::string& filename)
    {
        size_t len = filename.size();
        if( len < 4 )
            return false;
        return filename[len-4] == '.' && filename[len-3] == 'd' && filename[len-2] == 'a' && filename[len-1] == 'e';
    }
    static bool IsValidCharInName(char c) {
        return isalnum(c) || c == '_' || c == '-' || c == '.';
    }
    static bool IsValidName(const string& s) {
        if( s.size() == 0 )
            return false;
        return count_if(s.begin(), s.end(), IsValidCharInName) == (int)s.size();
    }

    boost::shared_ptr<RaveDatabase> _pdatabase;

    std::vector<RobotBasePtr> _vecrobots;  ///< robots (possibly controlled)
    std::vector<KinBodyPtr> _vecbodies; ///< all objects that are collidable (includes robots)

    list<ProblemInstancePtr> _listProblems; ///< problems loaded in the environment

    dReal _fDeltaSimTime;                ///< delta time for simulate step
    uint64_t _nCurSimTime;                    ///< simulation time since the start of the environment
    uint64_t _nSimStartTime;
    int _nBodiesModifiedStamp; ///< incremented every tiem bodies vector is modified
    bool _bRealTime;

    CollisionCheckerBasePtr _pCurrentChecker;
    PhysicsEngineBasePtr _pPhysicsEngine;
    RaveViewerBasePtr _pCurrentViewer;
    
    int _nEnvironmentIndex;               ///< next network index
    std::map<int, KinBodyWeakPtr> _mapBodies; ///< a map of all the bodies in the environment. Controlled through the KinBody constructor and destructors
    
    boost::shared_ptr<boost::thread> _threadSimulation;                  ///< main loop for environment simulation

    mutable EnvironmentMutex _mutexEnvironment;      ///< protects internal data from multithreading issues

    mutable boost::mutex _mutexEnvironmentIds;  ///< protects _vecbodies/_vecrobots from multithreading issues
    mutable boost::mutex _mutexBodies;  ///< protects _mapBodies from multithreading issues
    mutable boost::mutex _mutexProblems; ///< lock when managing problems

    vector<KinBody::BodyState> _vPublishedBodies;
    vector<string> _vplugindirs, _vdatadirs;
    string _homedirectory;

    list<InterfaceBasePtr> _listOwnedObjects;

    std::list<CollisionCallbackFn> _listRegisteredCollisionCallbacks; ///< see EnvironmentBase::RegisterCollisionCallback

    bool _bDestroying;              ///< destroying envrionment, so exit from all processes
    bool _bDestroyed;               ///< environment has been destroyed
    bool _bEnableSimulation;        ///< enable simulation loop

    friend class EnvironmentXMLReader;
};

#endif
