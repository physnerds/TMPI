// @(#)root/io:$Id$
 // Author: Amit Bashyal, August 2018
 
 /*************************************************************************
  * Copyright (C) 1995-2002, Rene Brun and Fons Rademakers.               *
  * All rights reserved.                                                  *
  *                                                                       *
  * For the licensing terms see $ROOTSYS/LICENSE.                         *
  * For the list of contributors see $ROOTSYS/README/CREDITS.             *
  *************************************************************************/
 
 /**
 \class TMPIFile TMPIFile.cxx
 \ingroup IO <-----------------NOT YET IMPLEMENTED------------>
 
 A TMPIFile uses TMemFile to create and merge rootfiles into a single TFile and writein a disk using MPI (Message Passing Interface) functionalities.
 */

#include "TMemFile.h"
#include "TError.h"
#include "TSystem.h"
#include "TROOT.h"
#include "TArrayC.h"
#include "TKey.h"
#include "TClass.h"
#include "TVirtualMutex.h"
#include "TMPIFile.h"
#include "TFileCacheWrite.h"
#include "TKey.h"
#include "TSystem.h"
#include "TMath.h"
#include "TTree.h"
#include <string>

ClassImp(TMPIFile);
//The constructor should be similar to TMemFile...

TMPIFile::TMPIFile(const char *name, char *buffer, Long64_t size,
		   Option_t *option,Int_t split,const char *ftitle,
		   Int_t compress):TMemFile(name,buffer,size,option,ftitle,compress),fColor(0),fRequest(0),fSendBuf(0),fSplitLevel(split){

  //Initialize MPI if it is not already initialized...
  int flag;
  MPI_Initialized(&flag);
  if(!flag) MPI_Init(&argc,&argv); 
  int global_size,global_rank;

  MPI_Comm_size(MPI_COMM_WORLD,&global_size);
  MPI_Comm_rank(MPI_COMM_WORLD,&global_rank);
  if(split!=0){
    if(2*split>global_size){
      SysError("TMPIFile"," Number of Output File is larger than number of Processors Allocated. Number of processors should be two times larger than outpts. For %d outputs at least %d should be allocated instead of %d\n.",split,2*split,global_size);
      exit(1);
    }
    int tot = global_size/split;
    if(global_size%split!=0){
      int n = global_size%split;
      tot=tot+1;
      fColor = global_rank/tot;
      row_comm = SplitMPIComm(MPI_COMM_WORLD,tot);
    }
    else{
      fColor = global_rank/tot;
      row_comm = SplitMPIComm(MPI_COMM_WORLD,tot);
    }
  }
  else{
    fColor = 0;
    row_comm = MPI_COMM_WORLD;
  }
}

TMPIFile::TMPIFile(const char *name,Option_t *option,Int_t split,const char *ftitle,
		   Int_t compress):TMemFile(name,option,ftitle,compress),fColor(0),fRequest(0),fSendBuf(0),fSplitLevel(split){
  int flag;
  MPI_Initialized(&flag);
  if(!flag)MPI_Init(&argc,&argv);
  int global_size,global_rank;
  MPI_Comm_size(MPI_COMM_WORLD,&global_size);
  MPI_Comm_rank(MPI_COMM_WORLD,&global_rank);
  if(split!=0){
    if(2*split>global_size){
      SysError("TMPIFile"," Number of Output File is larger than number of Processors Allocated. Number of processors should be two times larger than outpts. For %d outputs at least %d should be allocated instead of %d\n.",split,2*split,global_size);
      exit(1);
    }
    int tot = global_size/split;
    if(global_size%split!=0){
      int n = global_size%split;
      tot=tot+1;
      fColor = global_rank/tot;
      row_comm = SplitMPIComm(MPI_COMM_WORLD,tot);
    }
    else{
      fColor = global_rank/tot;
      row_comm = SplitMPIComm(MPI_COMM_WORLD,tot);
    }
  }
  else{
    fColor = 0;

    row_comm = MPI_COMM_WORLD;
  }
}
TMPIFile::~TMPIFile(){
  Close();
  fRequest=0;

}
//Function to allocate number of Allocators
MPI_Comm TMPIFile::SplitMPIComm(MPI_Comm source, int comm_no){
  int source_rank,source_size;
  MPI_Comm row_comm;
  MPI_Comm_rank(source,&source_rank);
  MPI_Comm_size(source,&source_size);
  if(comm_no>source_size){
    SysError("TMPIFile","number of sub communicators larger than mother size");
    exit(1);
  }
  int color = source_rank/comm_no;
  MPI_Comm_split(source,color,source_rank,&row_comm);
  return row_comm;
}


//*******************MASTERS' FUNCTIONS********************************


void TMPIFile::UpdateEndProcess(){
  fEndProcess=fEndProcess+1;
}


void TMPIFile::RunCollector(bool cache)
{
  //by this time, MPI should be initialized...
  int rank,size;
  MPI_Comm_rank(row_comm,&rank);
  MPI_Comm_size(row_comm,&size);
  ReceiveAndMerge(cache,row_comm,rank,size);
}
//*******************Collector's main function*******************888
void TMPIFile::ReceiveAndMerge(bool cache,MPI_Comm comm,int rank,int size){
  if(!this->IsCollector())return;
  this->GetRootName();
  THashTable mergers;
  // int empty_buff=0;
  int counter=1;
  while(fEndProcess!=size-1){
    char *buf;
    int count;
  MPI_Status status;
  MPI_Probe(MPI_ANY_SOURCE,MPI_ANY_TAG,comm,&status);
  MPI_Get_count(&status,MPI_CHAR,&count);
  int number_bytes = sizeof(char)*count;
  buf = new char[number_bytes];
  int source = status.MPI_SOURCE;
  int tag = status.MPI_TAG;
  if(number_bytes==0){
    //empty buffer is a worker's last send request....
    this->UpdateEndProcess();

     MPI_Recv(buf,number_bytes,MPI_CHAR,source,tag,comm,MPI_STATUS_IGNORE); 
  }
  else{

    MPI_Recv(buf,number_bytes,MPI_CHAR,source,tag,comm,MPI_STATUS_IGNORE); 
    
    Int_t client_Id =counter-1; 
    TMemFile *infile = new TMemFile(fMPIFilename,buf,number_bytes,"UPDATE"); 
    // const Float_t clientThreshold = 0.75;
    ParallelFileMerger *info = (ParallelFileMerger*)mergers.FindObject(fMPIFilename);
    if(!info){
      info = new ParallelFileMerger(fMPIFilename,cache);
      mergers.Add(info);
    }
    if(R__NeedInitialMerge(infile)){

      info->InitialMerge(infile);

    }
    info->RegisterClient(client_Id,infile);
    //    if(info->NeedMerge(clientThreshold)){
      info->Merge();
      // }
    infile = 0;
  
    TIter next(&mergers);
    while((info = (ParallelFileMerger*)next())){
      //  if(info->NeedFinalMerge()){ //not needed for TMPIFile. Every sent buffer is merged.
	info->Merge();
	//  }
    }
    counter=counter+1;
  }
   delete [] buf;
  }
  if(fEndProcess==size-1){
    mergers.Delete();

    return;
  }
  }


Bool_t TMPIFile::R__NeedInitialMerge(TDirectory *dir)
{
  if (dir==0) return kFALSE;
  TIter nextkey(dir->GetListOfKeys());
  TKey *key;
  while( (key = (TKey*)nextkey()) ) {
    TClass *cl = TClass::GetClass(key->GetClassName());
    const char *classname = key->GetClassName();
    if (cl->InheritsFrom(TDirectory::Class())) {
      TDirectory *subdir = (TDirectory *)dir->GetList()->FindObject(key->GetName());
      if (!subdir) {
	subdir = (TDirectory *)key->ReadObj();
      }
      if (R__NeedInitialMerge(subdir)) {
	return kTRUE;
      }
    } else {
      if (0 != cl->GetResetAfterMerge()) {
	return kTRUE;
      }
    }
  }
  return kFALSE;
}

TMPIFile::ParallelFileMerger::ParallelFileMerger(const char *filename,Bool_t writeCache):fFilename(filename),fClientsContact(0),fMerger(kFALSE,kTRUE)
{
  fMerger.SetPrintLevel(0);
  fMerger.OutputFile(filename,"RECREATE");
  if(writeCache)new TFileCacheWrite(fMerger.GetOutputFile(),32*1024*1024);

}
//And the destructor....
TMPIFile::ParallelFileMerger::~ParallelFileMerger()
{
  for(ClientColl_t::iterator iter = fClients.begin();
      iter != fClients.end();++iter)delete iter->fFile;

}
ULong_t TMPIFile::ParallelFileMerger::Hash()const{
  return fFilename.Hash();
}
const char *TMPIFile::ParallelFileMerger::GetName()const{
  return fFilename;
}
Bool_t TMPIFile::ParallelFileMerger::InitialMerge(TFile *input)
{
      // Initial merge of the input to copy the resetable object (TTree) into the output
      // and remove them from the input file.
  fMerger.AddFile(input);
  Bool_t result = fMerger.PartialMerge(TFileMerger::kIncremental | TFileMerger::kResetable);
  tcl.R__DeleteObject(input,kTRUE);
  return result;
}
Bool_t TMPIFile::ParallelFileMerger::Merge()
{
  tcl.R__DeleteObject(fMerger.GetOutputFile(),kFALSE); //removing object that cannot be incrementally merged and will not be reset by the client code..
  for(unsigned int f = 0; f<fClients.size();++f){
    fMerger.AddFile(fClients[f].fFile);
  }
  Bool_t result = fMerger.PartialMerge(TFileMerger::kAllIncremental);
  // Remove any 'resetable' object (like TTree) from the input file so that they will not
  // be re-merged.  Keep only the object that always need to be re-merged (Histograms).
  for(unsigned int f = 0 ; f < fClients.size(); ++f) {
    if (fClients[f].fFile) {
      tcl.R__DeleteObject(fClients[f].fFile,kTRUE);
    } else {
      // We back up the file (probably due to memory constraint)
      TFile *file = TFile::Open(fClients[f].fLocalName,"UPDATE");
      tcl.R__DeleteObject(file,kTRUE); // Remove object that can be incrementally merge and will be reset by the client code.
      file->Write();
      delete file;
    }
  }
  fLastMerge = TTimeStamp();
  fNClientsContact = 0;
  fClientsContact.Clear();

  return result;
}
void TMPIFile::ParallelFileMerger::RegisterClient(UInt_t clientID,TFile *file){
  // Register that a client has sent a file.

  ++fNClientsContact;
  fClientsContact.SetBitNumber(clientID);
   TClientInfo ntcl(std::string(fFilename).c_str(),clientID);					  
  if (fClients.size() < clientID+1) {
     fClients.push_back(ntcl);
  }
  fClients[clientID].Set(file);
}

Bool_t TMPIFile::ParallelFileMerger::NeedMerge(Float_t clientThreshold){
  // Return true, if enough client have reported
  //in case of TMPIFile this happens everytime a client/worker sends the buffer (tested).

  if (fClients.size()==0) {
    return kFALSE;
  }

  // Calculate average and rms of the time between the last 2 contacts.
  Double_t sum = 0;
  Double_t sum2 = 0;
  for(unsigned int c = 0 ; c < fClients.size(); ++c) {
    sum += fClients[c].fTimeSincePrevContact;
    sum2 += fClients[c].fTimeSincePrevContact*fClients[c].fTimeSincePrevContact;
  }
  Double_t avg = sum / fClients.size();
  Double_t sigma = sum2 ? TMath::Sqrt( sum2 / fClients.size() - avg*avg) : 0;
  Double_t target = avg + 2*sigma;
  TTimeStamp now;
  if ( (now.AsDouble() - fLastMerge.AsDouble()) > target) {
    return kTRUE;
  }
  Float_t cut = clientThreshold * fClients.size();
  return fClientsContact.CountBits() > cut  || fNClientsContact > 2*cut;

}
Bool_t TMPIFile::ParallelFileMerger::NeedFinalMerge()
{

  return fClientsContact.CountBits()>0;
}

//************************************************************************//


 void TMPIFile::R__MigrateKey(TDirectory *destination, TDirectory *source)
{
if (destination==0 || source==0) return;
TIter nextkey(source->GetListOfKeys());
   TKey *key;
   while( (key = (TKey*)nextkey()) ) {
      TClass *cl = TClass::GetClass(key->GetClassName());
      if (cl->InheritsFrom(TDirectory::Class())) {
         TDirectory *source_subdir = (TDirectory *)source->GetList()->FindObject(key->GetName());
         if (!source_subdir) {
            source_subdir = (TDirectory *)key->ReadObj();
         }
         TDirectory *destination_subdir = destination->GetDirectory(key->GetName());
         if (!destination_subdir) {
            destination_subdir = destination->mkdir(key->GetName());
         }
         R__MigrateKey(destination,source);
      } else {
         TKey *oldkey = destination->GetKey(key->GetName());
         if (oldkey) {
            oldkey->Delete();
            delete oldkey;
         }
         TKey *newkey = new TKey(destination,*key,0 /* pidoffset */); 
         destination->GetFile()->SumBuffer(newkey->GetObjlen());
         newkey->WriteFile(0);
         if (destination->GetFile()->TestBit(TFile::kWriteError)) {
            return;
         }
      }
   }
   destination->SaveSelf();
}

void TMPIFile::R__DeleteObject(TDirectory *dir, Bool_t withReset)
{
   if (dir==0) return;

 TIter nextkey(dir->GetListOfKeys());
   TKey *key;
   while( (key = (TKey*)nextkey()) ) {
      TClass *cl = TClass::GetClass(key->GetClassName());
      if (cl->InheritsFrom(TDirectory::Class())) {
         TDirectory *subdir = (TDirectory *)dir->GetList()->FindObject(key->GetName());
         if (!subdir) {
            subdir = (TDirectory *)key->ReadObj();
         }
         R__DeleteObject(subdir,withReset);
      } else {
         Bool_t todelete = kFALSE;
         if (withReset) {
            todelete = (0 != cl->GetResetAfterMerge());
         } else {
            todelete = (0 ==  cl->GetResetAfterMerge());
         }
         if (todelete) {
            key->Delete();
            dir->GetListOfKeys()->Remove(key);
            delete key;
         }
      }
   }
}

Bool_t TMPIFile::IsCollector(){
  Bool_t coll=false;
  int rank=this->GetMPILocalRank();
  if(rank==0)coll=true;
  return coll;
}
//*************************END OF MASTERS' FUNCTIONS******************//

//**************************START OF WORKER'S FUNCTIONS******************//
void TMPIFile::CreateBufferAndSend(bool cache,MPI_Comm comm)
{
  int rank,size;
  const char* _filename = this->GetName();
  this->Write();
  MPI_Comm_rank(comm,&rank);
  MPI_Comm_size(comm,&size);
  if(rank==0)return;
  int count =  this->GetSize();
   fSendBuf = new char[count];
  this->CopyTo(fSendBuf,count); 
  MPI_Isend(fSendBuf,count,MPI_CHAR,0,fColor,comm,&fRequest);

}
void TMPIFile::CreateEmptyBufferAndSend(bool cache,MPI_Comm comm)
{
  if(fRequest){
    MPI_Wait(&fRequest,MPI_STATUS_IGNORE);
    fRequest=0;
    delete[] fSendBuf;
  }
  int rank,size;
  MPI_Comm_rank(comm,&rank);
  MPI_Comm_size(comm,&size);
  if(rank==0)return;
  int sent= MPI_Send(fSendBuf,0,MPI_CHAR,0,fColor,comm);
  if(sent)delete[] fSendBuf;
}

//*************END OF WORKER'S FUNCTIONS***************************


//Synching defines the communication method between worker/collector
void TMPIFile::Sync(bool cache){
  int rank,size;
  MPI_Comm_rank(row_comm,&rank);
  MPI_Comm_size(row_comm,&size);
  //check if the previous send request is accepted by master.
  if(!fRequest){//if accpeted create and send current batch
     CreateBufferAndSend(cache,row_comm);
   }
   else{
     //if not accepted wait until received by master
      MPI_Wait(&fRequest,MPI_STATUS_IGNORE);
      if(fRequest) delete[] fSendBuf; //empty the buffer once received by master
      //Reset the frequest once accepted by master and send new buffer
      fRequest=0; 
      CreateBufferAndSend(cache,row_comm);
   }
}

void TMPIFile::MPIClose(bool cache){
    int rank,size;
    MPI_Comm_rank(row_comm,&rank);
    MPI_Comm_size(row_comm,&size);
    CreateEmptyBufferAndSend(cache,row_comm);
  //workers need to wait other workers to reach the end of job.....
    MPI_Barrier(row_comm);
   //okay once they reach the buffer...just close it.
    this->Close();
  
}


void TMPIFile::GetRootName()
{
  std::string _filename = this->GetName();

  int found = _filename.rfind(".root");
  if(found != std::string::npos)_filename.resize(found);
  const char* _name = _filename.c_str();
  sprintf(fMPIFilename,"%s_%d.root",_name,fColor);

}

Int_t TMPIFile::GetMPIGlobalRank(){
  int flag;
  int rank;
  MPI_Initialized(&flag);
  if(flag)MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  else{rank=-9999;}
  return rank;
}

Int_t TMPIFile::GetMPILocalRank(){
  int flag,rank;
  MPI_Initialized(&flag);
  if(flag)MPI_Comm_rank(row_comm,&rank);
  else(rank=-9999);
  return rank;
}

Int_t TMPIFile::GetMPIGlobalSize(){
  int flag,size;
  MPI_Initialized(&flag);
  if(flag)MPI_Comm_size(MPI_COMM_WORLD,&size);
  else{size=-1;}
  return size;
}
Int_t TMPIFile::GetMPILocalSize(){
  int flag,size;
  MPI_Initialized(&flag);
  if(flag)MPI_Comm_size(row_comm,&size);
  else{size=-1;}
  return size;
}

Int_t TMPIFile::GetMPIColor(){
  return fColor;
}

Int_t TMPIFile::GetSplitLevel(){
  return fSplitLevel;
}





