#include <iostream>
#include <string>
#include <fstream>
#include <math.h>
#include <stdlib.h>
#include <sstream>
#include "navier.h"
#include "tools.h"
#include "terminalPrinter.h"


//length of options pool


using namespace std;

void NavierStokesSolver::NSSolve( )
{
	int iter;
	double ResMax,ResMax0, tstart,tend;
	tstart = ttime( );
    for( step=1; step<=MaxStep; step++ )
    {
		if( (step-1)%10==0 )printer->printSectionHead(cur_time);
		if( !IfSteady ){
			cur_time += dt;
			SaveTransientOldData( );
		}

		// outer iteration, drive residual to zero for every physical time step
		for( iter=1; iter<MaxOuterStep; iter++ ){
			CalculateVelocity ( );
				// Output2Tecplot ( );
			CalculatePressure ( );

			// scalar transportation
			//1. turbulence model
			if( TurModel==1  ) UpdateTurKEpsilon( );
			//2. energy couple
			if( SolveEnergy  ) UpdateEnergy ( );
			//3. species transport
			if( SolveSpecies ) UpdateSpecies( );
			//4. other physical models
			//    e.g., condensation, combustion, wall heat conduction

			ResMax = vec_max( Residual,10 );
			if( IfSteady ) 
				break;
			else  // unsteady
			{
				if( iter==1    ) ResMax0 = ResMax;
				if( ResMax<1.e-4 || ResMax0/(ResMax+1.e-16)>1000. ){
					printer->printStepStatus(step,iter,cur_time,dt,ResMax);
					break; // more reasonal to break : order drop 2
				}
			}
		}
		if( step%noutput==0 ){
			tend   = ttime( );
			//cout<<"  spend time "<<tend-tstart<<" seconds"<<endl;
			tstart = tend;
			//cout<<"  output to tecplot ......."<<endl;
			if( outputFormat==0 ) Output2Tecplot ( );  // exit(0);
			if( outputFormat==1 ) Output2VTK     ( );
			WriteBackupFile( );
		}
		if( IfSteady ){
			printer->printSteadyStatus(step,ResMax);
			if( ResMax<ResidualSteady )break;
		}
		else if( cur_time >= total_time )break;
	}
	printer->printEnding();
	//if( outputFormat==0 ) Output2Tecplot ( );
	//if( outputFormat==1 ) Output2VTK     ( );
	//WriteBackupFile( );
}


/*******************************************************/
//	fetch param and mesh Data From root
//	MPI_Broadcast routine
//	collective
/*******************************************************/
void NavierStokesSolver::broadcastSolverParam(){
	PetscPrintf(dataPartition->comm,"begin broadcast basic parameter\n");
	int sourceRank = root.rank;

	MPI_Barrier(dataPartition->comm);

	//------bool option pool
	MPI_Bcast(iOptions,INT_OPTION_NO,MPI_INT,sourceRank,dataPartition->comm);

	//------double option pool
	MPI_Bcast(dbOptions,DB_OPTION_NO,MPI_DOUBLE,sourceRank,dataPartition->comm);


	//------other constant, nProcess, gridList, nCell etc.
	
	dataPartition->gridList = new int[dataPartition->nProcess];
	int* _sendbuf = NULL;
	int* _sendbuf1 = NULL;

	if(root.rootgridList!=NULL){//root only
		dataPartition->nProcess = root.rootgridList->size();
		dataPartition->nGlobal = root.rootNGlobal;
		for(int i=0;i!=dataPartition->nProcess;++i)
			dataPartition->gridList[i] = root.rootgridList->at(i);

		_sendbuf = new int[dataPartition->nProcess];
		_sendbuf1 = new int[dataPartition->nProcess];

		for(int i=0;i!=dataPartition->nProcess;++i)
			_sendbuf[i] = root.rootNCells->at(i);

		for(int i=0;i!=dataPartition->nProcess;++i)
			_sendbuf1[i] = root.nodesPool[i].size();

	}

	MPI_Bcast(&(dataPartition->nProcess),1,MPI_INT,sourceRank,dataPartition->comm);//nProcess

	MPI_Bcast(&(dataPartition->nGlobal),1,MPI_INT,sourceRank,dataPartition->comm);//nProcess

	MPI_Bcast(dataPartition->gridList, dataPartition->nProcess, MPI_INT, sourceRank, dataPartition->comm);//gridlist

	//scatter: sendbuf, sendcount(number of element to EACH process), sendtype, recvbuf, recvcount, recvtype, rootrank, communicator
	MPI_Scatter(_sendbuf,1, MPI_INT, &(this->Ncel), 1, MPI_INT,root.rank,dataPartition->comm  );//nCell

	MPI_Scatter(_sendbuf1,1, MPI_INT, &(this->Nvrt), 1, MPI_INT,root.rank,dataPartition->comm  );//nVert

	//some configuration...
	int _nlocalElement = dataPartition->gridList[dataPartition->comRank];

	this->Nbnd = _nlocalElement - Ncel;
	dataPartition->nLocal = Ncel;

	PetscPrintf(dataPartition->comm,"complete broadcast basic parameter\n");

	/*
	printf("check parameter\n");
	printf("rank %d, nProcess %d, nLocal %d, nGlobal %d, nCell %d, nBnd %d\n",
			dataPartition->comRank, dataPartition->nProcess, dataPartition->nLocal, dataPartition->nGlobal, Ncel, Nbnd);
	printf("vert------>%d\n",Nvrt);
	*/
	delete []_sendbuf;
	delete []_sendbuf1;
}



/*******************************************************/
//	fetch param and mesh Data From root
//	MPI_Broadcast routine
//	collective
//
//	parameter is for input, pass NULL to this routine
//	this routien will malloc buffer
//	it is the USERs responsibility to free this buffer!
/*******************************************************/
#define SEND_TAG_ELEMENT 0
#define SEND_TAG_VERTEX 100
#define SEND_TAG_INTERFACEINFO 200
void NavierStokesSolver::scatterGridFile(int** elemBuffer, double** vertexBuffer, int** interfaceBuffer){
	//scatterv and multiple send operation are both good for this subroutine
	MPI_Request* sendRequests = NULL;	
	MPI_Request* recvRequtests = NULL;
	double** buffer1       = NULL; //vertex send buffer
	int** buffer2          = NULL; //elements send buffer
	int** buffer3          = NULL; //interfaces send buffer
	
	int ierr = 0;
	
	int _size = 0;
	int _sendSize[3] = {0,0,0};
	int _recvSize[3] = {0,0,0};

	MPI_Barrier(dataPartition->comm);

	if(dataPartition->comRank==root.rank){//root //sending side...
		_size = dataPartition->nProcess;
		sendRequests = new MPI_Request[ 3 * _size ];
		buffer1 = new double* [_size];
		buffer2 = new int* [_size];
		buffer3 = new int* [_size];

		for(int pid = 0;pid!=_size;++pid){
			//printf("root: sending geometry to grid: %d\n",pid);
			_sendSize[0] = root.getVertexSendBuffer(dataPartition, pid, buffer1 + pid);
			_sendSize[1] = root.getElementSendBuffer(dataPartition,pid, buffer2+pid);
			_sendSize[2] = root.getInterfaceSendBuffer(dataPartition, pid, buffer3 + pid);
			//printf("prepared buffer for pid %d, %d,%d,%d\n",pid,_sendSize[0],_sendSize[1],_sendSize[2]);

			//non-blocking buffered send
			MPI_Bsend(_sendSize,3,MPI_INT,pid,244,dataPartition->comm);

			//send vertex	
			MPI_Issend(buffer1[pid], _sendSize[0], MPI_DOUBLE,
					pid,		     	     //dest
					SEND_TAG_VERTEX,	     //send tag
					dataPartition->comm,
					sendRequests+3*pid+0); 	     //return request
			//send elements
			MPI_Issend(buffer2[pid], _sendSize[1], MPI_INT,
					pid,		      	     //dest
					SEND_TAG_ELEMENT,	     //send tag
					dataPartition->comm,
					sendRequests+3*pid+1); 	     //return request

			//send interfaceinfo
			MPI_Issend(buffer3[pid], _sendSize[2], MPI_INT,
					pid,			     //dest
					SEND_TAG_INTERFACEINFO,  //send tag
					dataPartition->comm,
					sendRequests+3*pid+2); 	     //return request
		}			




	} 

	//receiveing side ...
	//root is sending to itself...
	//printf("rank: %d is receiving geometry...\n",dataPartition->comRank);
	recvRequtests = new MPI_Request[ 3 ];
	//blocking receive!
	MPI_Recv(_recvSize,3,MPI_INT,root.rank,244,dataPartition->comm,MPI_STATUS_IGNORE);

	
	printf("rank:%d recving buffersize: %d, %d, %d\n",dataPartition->comRank,_recvSize[0],_recvSize[1],_recvSize[2]);

	*vertexBuffer = new double[_recvSize[0]];
	*elemBuffer = new int[_recvSize[1]];
	*interfaceBuffer = new int[_recvSize[2]];

	//recv elements
	MPI_Irecv(*vertexBuffer,_recvSize[0], MPI_DOUBLE,
			root.rank,			//source
			SEND_TAG_VERTEX,
			dataPartition->comm,
			recvRequtests+0); 

	MPI_Irecv(*elemBuffer,_recvSize[1], MPI_INT,
			root.rank,			//source
			SEND_TAG_ELEMENT,		//tag
			dataPartition->comm,
			recvRequtests+1); 			//return value
		//return value
	MPI_Irecv(*interfaceBuffer,_recvSize[2], MPI_INT,
			root.rank,			//source
			SEND_TAG_INTERFACEINFO,
			dataPartition->comm,
			recvRequtests+2); 			//return value

	MPI_Status status[3];
	ierr = MPI_Waitall(3,recvRequtests,status);
	if(ierr!=MPI_SUCCESS){
		char temp[256];
		sprintf(temp,"MPI receive failure in geometry transfering\n");
		throw runtime_error(temp);
	}else{
		printf("rank: %d received geometry buffer\n",dataPartition->comRank);
	}	
	delete []recvRequtests;


	if(dataPartition->comRank == root.rank){//check completion in root
		ierr = MPI_Waitall(3*_size,sendRequests,MPI_STATUS_IGNORE);
		if(ierr!=MPI_SUCCESS){
			throw runtime_error("MPI send failure in geometry transfering!\n ");
		}

		for(int i=0;i!=_size;++i){
			delete []buffer1[i];
			delete []buffer2[i];
			delete []buffer3[i];
		}
		delete []buffer1;
		delete []buffer2;
		delete []buffer3;
		delete []sendRequests;
	}
	MPI_Barrier(dataPartition->comm);
	PetscPrintf(dataPartition->comm,"complete scatter grid\n");
}



/*******************************************************/
//	inition of MPI related variables
//	the inition of Params can be overwritten by Command line option and input parameters
//	this function is root ONLY
/*******************************************************/
void NavierStokesSolver::initSolverParam() 
{
	if(dataPartition->comRank != root.rank) return; //root ONLY


	int i;
	// default values, can be reset in initflow
	root.printer = new TerminalPrinter;
	printer->printStarter();
	MaxStep      = 10000 ;
	MaxOuterStep = 50  ;
	IfReadBackup = false;
	IfSteady     = true ;  ResidualSteady= 1.e-6;
	TurModel     = 0;
	DensityModel = 0;
	SolveEnergy  = false;
	SolveSpecies = false;
	Nspecies     = 0;

	PressureReference= 1.01325e5; cellPressureRef=0;
	gama			 = 1.4;
	ga1				 = 0.4;
	cp				 = 1006.;
	cv				 = cp/gama;
	prl				 = 0.72;
	prte			 = 0.9;
	Rcpcv			 = cp-cv;
	TempRef			 = 273.15;
	for( i=0; i<3; i++ )
		gravity[i] = 0.;

	//-- numerical scheme
	// relaxation factor
	URF[0]= 0.6;  // u
	URF[1]= 0.6;  // v
	URF[2]= 0.6;  // w
	URF[3]= 0.5;  // p
	URF[4]= 0.8;  // T
	URF[5]= 0.8;  // k
	URF[6]= 0.8;  // e
	URF[7]= 0.8;  // scalar
	limiter = 0;

	total_time   = 0.    ;
	dt           = 1.0   ;    // no meaning in steady case
	TimeScheme   = 1     ;

	noutput  = 1;
	outputFormat = 0;  // 0 for tecplot; 1 for vtk

	// boundary
	pin = 0.; 
	pout= 0.;

	/*
	// specific problem parameters
	// cylinder
	IfSteady     = true ;  dt=0.1;  TimeScheme=1;
	SolveEnergy  = false ;
	TurModel     = 1     ;
	DensityModel = 0     ;
	*/

	/*
	// heat exchanger
	IfSteady     = true ;
	MaxStep      = 500  ;
	SolveEnergy  = true ;
	TurModel     = 1    ;
	DensityModel = 1    ; */


	// init parameters from param.in
	
	ReadParamFile( );

	// some parameters check work, e.g. 
}


void NavierStokesSolver::InitFlowField( ){
	int i;

	if( IfReadBackup ) 
		ReadBackupFile( );
	else{
	for( i=0; i<Ncel; i++ )
	{
		Un[i] = initvalues[0];
		Vn[i] = initvalues[1];
		Wn[i] = initvalues[2];
		Rn[i] = initvalues[3];
		Tn[i] = initvalues[4];
		Pn[i] = 0.;
		if( DensityModel== 1 ) Rn[i]= (Pn[i]+PressureReference)/(Rcpcv*Tn[i]);

		VisLam[i]= initvalues[5]; // 0.6666667e-2;  // 1.458e-6 * pow(Tn[i],1.5) /(Tn[i]+110.5) ;
		VisTur[i]= 0.;
		if( TurModel==1 )
		{
			TE[i]    = initvalues[6];  // 1.e-4*(Un[i]*Un[i]+Vn[i]*Vn[i]+Wn[i]*Wn[i]);
			ED[i]    = initvalues[7];    // TurKEpsilonVar::Cmu * pow(TE[i],1.5) / 1.;
			VisTur[i]= Rn[i]*TurKEpsilonVar::Cmu * TE[i]*TE[i]/(ED[i]+SMALL);
		}
	}
	}
	// change grid boundary to actual boundary
	int rid;  // rtable[10]={0,1,2,3,4,0},
	for( i=0; i<Nbnd; i++ )
	{
		rid = Bnd[i].rid;
		Bnd[i].rid = rtable[ rid ];
	}
	

	for( i=0;i<Nfac;i++ )
		RUFace[i] = 0.;
}


void NavierStokesSolver::SaveTransientOldData( )
{
	int i,j;
	if(      TimeScheme==1 ){  // Euler forwards
		for(i=0;i<Ncel;i++)
		{
			Rnp [i]= Rn[i];
			Unp [i]= Un[i];
			Vnp [i]= Vn[i];
			Wnp [i]= Wn[i];
			Tnp [i]= Tn[i];
			TEp [i]= TE[i];
			EDp [i]= ED[i];
			for( j=0; j<Nspecies; j++ )
				RSnp[i][j]= RSn[i][j];
			if( TurModel==1 )
			{
				TEp[i]= TE[i];
				EDp[i]= ED[i];
			}
		}
	}
	else if( TimeScheme==2 ){  // 2nd order BDF
		for(i=0;i<Ncel;i++)
		{
			//?? this may go wrong if compiler does not execute from right to left ??
			Rnp2[i]= Rnp[i]= Rn[i];
			Unp2[i]= Unp[i]= Un[i];
			Vnp2[i]= Vnp[i]= Vn[i];
			Wnp2[i]= Wnp[i]= Wn[i];
			Tnp2[i]= Tnp[i]= Tn[i];
			TEp2[i]= TEp[i]= TE[i];
			EDp2[i]= EDp[i]= ED[i];
			for( j=0; j<Nspecies; j++ )
				RSnp2[i][j]= RSnp[i][j]= RSn[i][j];
			if( TurModel==1 )
			{
				TEp2[i]= TEp[i]= TE[i];
				EDp2[i]= EDp[i]= ED[i];
			}
		}
	}
	else
	{
		cout<<"No unsteady time advanced scheme? Are you kidding?"<<endl;
		exit(0);
	}
}



/***********************************************/
// 	CONSTRUCTOR !!!	
/***********************************************/
NavierStokesSolver::NavierStokesSolver():
	outputCounter(0), 
	printer(NULL), //define in init
	dataPartition(new DataPartition),
	root(0),			// the root rank is 0
	iOptions(new int[INT_OPTION_NO]),
	dbOptions(new double[DB_OPTION_NO]),
	//option sets
	//bool
	IfReadBackup		(iOptions[0]),
	IfSteady		(iOptions[1]),
	SolveEnergy		(iOptions[2]),
	SolveSpecies		(iOptions[3]),
	//int
	MaxOuterStep		(iOptions[4]),
	TurModel		(iOptions[5]),
	DensityModel		(iOptions[6]),
	limiter			(iOptions[7]),
	TimeScheme		(iOptions[8]),
	noutput			(iOptions[9]),
	outputFormat		(iOptions[10]),
	Nspecies		(iOptions[11]),
	cellPressureRef		(iOptions[12]),
	MaxStep			(iOptions[13]),
	//double
	PressureReference	(dbOptions[0]),
	gama			(dbOptions[1]),
	ga1			(dbOptions[2]),
	cp			(dbOptions[3]),
	cv			(dbOptions[4]),
	prl			(dbOptions[5]),
	prte			(dbOptions[6]),
	Rcpcv			(dbOptions[7]),
	TempRef			(dbOptions[8]),
	total_time		(dbOptions[9]),
	dt			(dbOptions[10]),
	uin			(dbOptions[11]),
	vin			(dbOptions[12]),
	win			(dbOptions[13]),
	roin			(dbOptions[14]),
	Tin			(dbOptions[15]),
	tein			(dbOptions[16]),
	edin			(dbOptions[17]),
	Twall			(dbOptions[18]),
	pin			(dbOptions[19]),
	pout			(dbOptions[20]),
	gravity			(dbOptions+21), //gravity components: 22,23,24
	URF			(dbOptions+24),  //URF 	24~31 //length 8

	//all put NULL to avoid wild pointer
	Vert(NULL),Face(NULL),Cell(NULL),Bnd(NULL), 
	Rn(NULL),Un(NULL),Vn(NULL),Wn(NULL),Pn(NULL),Tn(NULL),TE(NULL),ED(NULL),
	RSn(NULL),

	VisLam(NULL),VisTur(NULL),

	dPdX(NULL),dUdX(NULL),dVdX(NULL),dWdX(NULL),Apr(NULL),dPhidX(NULL),

	RUFace(NULL),

	BRo(NULL),BU(NULL),BV(NULL),BW(NULL),BPre(NULL),BTem(NULL),BRS(NULL),BTE(NULL),BED(NULL),

	Rnp(NULL),Unp(NULL),Vnp(NULL),Wnp(NULL),Tnp(NULL),TEp(NULL),EDp(NULL),RSnp(NULL),
	Rnp2(NULL),Unp2(NULL),Vnp2(NULL),Wnp2(NULL),Tnp2(NULL),TEp2(NULL),EDp2(NULL),RSnp2(NULL)
{
}


/***********************************************/
// 	DECONSTRUCTOR
/***********************************************/
NavierStokesSolver::~NavierStokesSolver()
{
	// output the result before error
	// Output2Tecplot ();
	
	cout<<"desturct object and free space"<<endl;
	// delete variables
   	delete [] Rn;
	delete [] Un;
	delete [] Vn;
	delete [] Wn;
	delete [] Pn;
	delete [] Tn;
	delete [] VisLam;
	delete [] VisTur;
	delete_Array2D( dPdX,Ncel,3 );
	delete_Array2D( dUdX,Ncel,3 );
	delete_Array2D( dVdX,Ncel,3 );
	delete_Array2D( dWdX,Ncel,3 );
	delete [] Apr;
	delete_Array2D( dPhidX,Ncel,3 );

	delete [] BRo ;
	delete [] BU  ;
	delete [] BV  ;
	delete [] BW  ;
	delete [] BTem;
	delete [] BPre;

	delete [] RUFace;
	/*****************THIS PART IS ADDED BY CHENXUYI*******************/
	delete printer;
	delete dataPartition;

	delete iOptions;
	delete dbOptions;
	printer = NULL;
	dataPartition = NULL;
	delete printer;
	/*
        V_Destr ( &bs );
	V_Destr ( &bu );
	V_Destr ( &bv );
	V_Destr ( &bw );
	V_Destr ( &bp );
	V_Destr ( &xsol );
	*/
}

