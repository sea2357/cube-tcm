#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <dlfcn.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include "data_type.h"
#include "alloc.h"
#include "list.h"
#include "attrlist.h"
#include "memfunc.h"
#include "basefunc.h"
#include "struct_deal.h"
#include "crypto_func.h"
#include "channel.h"
#include "memdb.h"
#include "message.h"
#include "connector.h"
#include "ex_module.h"
#include "sys_func.h"
#include "tcm_constants.h"
#include "app_struct.h"
#include "pik_struct.h"
#include "sm3.h"
#include "sm4.h"

#include "tspi.h"
#include "tsmd.h"
#include "tspi_internal.h"


static  BYTE Buf[DIGEST_SIZE*32];
static  BYTE Output[DIGEST_SIZE*32];
Record_List sessions_list;
TCM_PUBKEY * pubEK;
TCM_SECRET ownweAuth;
TCM_SECRET smkAuth;

struct tsm_object_list
{
	int object_type;
	Record_List object_list;
}__attribute__((packed));

Record_List entitys_list;

static CHANNEL * vtcm_caller;

extern BYTE * CAprikey;
extern unsigned long * CAprilen;
extern BYTE * CApubkey;

static key_t sem_key;
static int semid;

static key_t shm_share_key;
static int shm_share_id;
static int shm_size;
char * pathname="/tmp";

enum tsmd_context_init_state
{
	TSMD_CONTEXT_INIT_START=0x01,
	TSMD_CONTEXT_INIT_GETREQ,
	TSMD_CONTEXT_INIT_BUILDCHANNEL,
	TSMD_CONTEXT_INIT_WAITCHANNEL,
	TSMD_CONTEXT_INIT_FINISH,
	TSMD_CONTEXT_INIT_TIMEOUT
};

enum tsmd_context_state
{
	TSMD_CONTEXT_INIT=0x01,
	TSMD_CONTEXT_BUILD,
	TSMD_CONTEXT_APICALL,
	TSMD_CONTEXT_SENDDATA,
	TSMD_CONTEXT_RECVDATA,
	TSMD_CONTEXT_APIRETURN,
	TSMD_CONTEXT_CLOSE,
	TSMD_CONTEXT_ERROR
};

typedef struct tsmd_context_struct
{
	int count;
	UINT32 handle;
	int shmid;
	enum tsmd_context_state state;
		
	int tsmd_API;
	int curr_step;
	int shm_size;
	void * tsmd_context; 	
	BYTE * tsmd_send_buf;
	BYTE * tsmd_recv_buf;
	CHANNEL * tsmd_API_channel;
}__attribute__((packed)) TSMD_CONTEXT;


struct tsmd_server_struct
{
	int curr_count;
	enum tsmd_context_init_state init_state;
	Record_List contexts_list;
}__attribute__((packed));

static struct tsmd_server_struct server_context;

TSMD_CONTEXT * Find_TsmdContext(UINT32 tsmd_handle)
{
  Record_List * record;
  Record_List * head;
  struct List_head * curr;
  TSMD_CONTEXT * tsmd_context;

  head=&(server_context.contexts_list.list);
  curr=head->list.next;

  while(curr!=head)
  {
    record=List_entry(curr,Record_List,list);
    tsmd_context=record->record;
    if(tsmd_context==NULL)
       return NULL;
    if(tsmd_context->handle==tsmd_handle)
        return tsmd_context;
    curr=curr->next;
  }
  return NULL;
}

TSMD_CONTEXT * Build_TsmdContext(int count, UINT32 tsmd_nonce)
{

  TSMD_CONTEXT * new_context=NULL;
  UINT32 new_handle;
 
  do{
	RAND_bytes(&new_handle,sizeof(new_handle));
	new_handle^=tsmd_nonce ;
	if(new_handle==0)
		continue; 
	if(new_handle==tsmd_nonce)
		continue;
  }while(new_context!=NULL);

  new_context=Dalloc0(sizeof(*new_context),NULL);
  if(new_context==NULL)
    return NULL;
  new_context->count=count;
  new_context->handle=new_handle;
  new_context->shmid=0;
  new_context->state=TSMD_CONTEXT_INIT;
  new_context->tsmd_API=0;
  new_context->curr_step=0;
  new_context->tsmd_context=NULL;
  new_context->tsmd_send_buf=Dalloc0(1024,NULL);
  new_context->tsmd_recv_buf=Dalloc0(1024,NULL);		
  new_context->tsmd_API_channel=NULL;
	
  // add authdata to the session_list

  Record_List * record = Calloc0(sizeof(*record));
  if(record==NULL)
    return -EINVAL;
  INIT_LIST_HEAD(&record->list);
  record->record=new_context;
  List_add_tail(&record->list,&server_context.contexts_list.list);
  return new_context;	
}



TCM_SESSION_DATA * Create_AuthSession_Data(TCM_ENT_TYPE * type,BYTE * auth,BYTE * nonce)
{
  TCM_SESSION_DATA * authdata=Dalloc0(sizeof(*authdata),NULL);
  if(authdata==NULL)
    return NULL;
  authdata->entityTypeByte=*type;
  Memcpy(authdata->nonceEven,nonce,TCM_HASH_SIZE);
  Memcpy(authdata->sharedSecret,auth,TCM_HASH_SIZE);
  return authdata;	
}

TCM_AUTHHANDLE Build_AuthSession(TCM_SESSION_DATA * authdata,void * tcm_out_data)
{
  BYTE auth[TCM_HASH_SIZE];
  struct tcm_out_APCreate * apcreate_out = tcm_out_data;
  authdata->SERIAL=apcreate_out->sernum;

  // Build shareSecret
  Memcpy(Buf,authdata->nonceEven,TCM_HASH_SIZE);
  Memcpy(authdata->nonceEven,apcreate_out->nonceEven,TCM_HASH_SIZE);
  Memcpy(Buf+TCM_HASH_SIZE,apcreate_out->nonceEven,TCM_HASH_SIZE);
  Memcpy(auth,authdata->sharedSecret,TCM_HASH_SIZE);
  sm3_hmac(auth,TCM_HASH_SIZE,Buf,TCM_HASH_SIZE*2,authdata->sharedSecret);

  if(authdata->entityTypeByte!=TCM_ET_NONE)
  {
    //	check the authcode

  }
  authdata->handle=apcreate_out->authHandle;
  // add authdata to the session_list

  Record_List * record = Calloc0(sizeof(*record));
  if(record==NULL)
    return -EINVAL;
  INIT_LIST_HEAD(&record->list);
  record->record=authdata;
  List_add_tail(&record->list,&sessions_list.list);
  return authdata->handle;	
}


TCM_SESSION_DATA * Find_AuthSession(TCM_ENT_TYPE type, TCM_AUTHHANDLE authhandle)
{
  Record_List * record;
  Record_List * head;
  struct List_head * curr;
  TCM_SESSION_DATA * authdata;

  head=&(sessions_list.list);
  curr=head->list.next;

  while(curr!=head)
  {
    record=List_entry(curr,Record_List,list);
    authdata=record->record;
    if(authdata==NULL)
      return NULL;
    if(type==0)
    {
      if(authdata->handle==authhandle)
        return authdata;
    }

    if(authdata->entityTypeByte==type)
    {
      if(type==TCM_ET_NONE)
        return authdata;
      if(authdata->handle==authhandle)
        return authdata;
    }
    curr=curr->next;
  }
  return NULL;
}

struct context_init * share_init_context;

int tsmd_init(void * sub_proc,void * para)
{
    int ret;
    struct tsmd_init_para * init_para=para;
    if(para==NULL)
	return -EINVAL;
    vtcm_caller=channel_find(init_para->channel_name);

    if(vtcm_caller==NULL)
    {
	print_cubeerr("tsm's tcm channel does not exist!,enter no tcm running state!\n");
	return 0;	
    }	

   INIT_LIST_HEAD(&sessions_list.list);
   sessions_list.record=NULL;

   INIT_LIST_HEAD(&server_context.contexts_list.list);
   server_context.contexts_list.record=NULL;
  

//   build the sem_key 
    sem_key = ftok(pathname,0x01);

    if(sem_key==-1)

    {
        printf("ftok sem_key error");
        return -1;
    }

    printf("sem_key=%d\n",sem_key) ;
    semid=semget(sem_key,1,IPC_CREAT|IPC_EXCL|0666);
    if(semid<0)
    {
	printf("open share semaphore failed!\n");
	return -EINVAL;
    }

   // build the share shm key
    shm_share_key = ftok(pathname,0x02);

    if(shm_share_key==-1)

    {
        printf("ftok shm_share_key error");
        return -1;
    }

    printf("shm_share_key=%d\n",shm_share_key) ;
    shm_size=sizeof(*share_init_context);
    shm_share_id=shmget(shm_share_key,shm_size,IPC_CREAT|IPC_EXCL|0666);
    if(shm_share_id<0)
    {
	printf("open share memory failed!\n");
	return -EINVAL;
    }
    share_init_context=(struct context_init *)shmat(shm_share_id,NULL,0);
		

    server_context.curr_count=1;
    INIT_LIST_HEAD(&server_context.contexts_list.list);
    server_context.init_state=TSMD_CONTEXT_INIT_START;
    share_init_context->count=server_context.curr_count;

    set_semvalue(semid,2);	
    return 0;
}

int tsmd_start(void * sub_proc,void * para)
{

    int ret;
    int retval;
    int i,j;
    int argv_offset;	
    char namebuffer[DIGEST_SIZE*4];


    static void * shm_share_addr;

    static CHANNEL * shm_channel;
    TSMD_CONTEXT * new_context=NULL;


    while(1)
    {
           usleep(time_val.tv_usec);

	    // do the tspi context init func
	   if(share_init_context->handle!=0)
	   {
		
		if(server_context.init_state==TSMD_CONTEXT_INIT_START)
		{
			server_context.init_state=TSMD_CONTEXT_INIT_GETREQ;
			new_context = Build_TsmdContext(share_init_context->count,share_init_context->handle);
			if(new_context==NULL)
				return -EINVAL;
    			static key_t shm_key;
			shm_key=ftok(pathname,new_context->count+0x02);
			if(shm_key == -1)
    			{
       				 printf("ftok shm_share_key error");
        			return -1;
    			}

    			printf("shm_key=%d\n",shm_key) ;
			new_context->shm_size=4096;
    			new_context->shmid=shmget(shm_key,new_context->shm_size,IPC_CREAT|IPC_EXCL|0666);
    			if(new_context->shmid<0)
    			{
				printf("open context share memory failed!\n");
				return -EINVAL;
    			}
			void * share_addr;
    			share_addr=shmat(new_context->shmid,NULL,0);
    			printf("shm_addr=%x\n",share_addr) ;

			Memset(namebuffer,0,DIGEST_SIZE);
			Memset(share_addr,0,new_context->shm_size);
			Strcpy(namebuffer,"channel");
			Itoa(new_context->count,namebuffer+Strlen(namebuffer));

			new_context->tsmd_API_channel = channel_register_fixmem(namebuffer,CHANNEL_RDWR|CHANNEL_FIXMEM,NULL,
                		new_context->shm_size/2,share_addr,share_addr+new_context->shm_size/2);
		
			share_init_context->handle=new_context->handle;	
			semaphore_v(semid,1);
		}
		else if(server_context.init_state==TSMD_CONTEXT_INIT_GETREQ)
		{
			if(share_init_context->count!=server_context.curr_count)
			{
				channel_inner_write(new_context->tsmd_API_channel,"TSMD",5);
				server_context.init_state=TSMD_CONTEXT_INIT_WAITCHANNEL;
			}
		}
		else if(server_context.init_state==TSMD_CONTEXT_INIT_WAITCHANNEL)
		{
			ret=channel_inner_read(new_context->tsmd_API_channel,Buf,5);
			if(ret==5)
			{	
				if(Strncmp(Buf,"TSPI",5)==0)
				{
					server_context.curr_count++;
					share_init_context->count=server_context.curr_count;	
					share_init_context->handle=0;
					semaphore_v(semid,2);
				}
				else
					return -EINVAL;
				server_context.init_state=TSMD_CONTEXT_INIT_START;
				new_context->state=TSMD_CONTEXT_BUILD;
			}
			else if(ret==0)
				continue;
			else
				return -EINVAL;
		}
	   }

	   // throughout  every context to find function call
           
	   ret=proc_each_tspicalls(sub_proc);
	   if(ret<0)
		break;
    }	
    return ret;
}

int proc_each_tspicalls(void * sub_proc)
{
	int ret;
	struct List_head * curr_head;
	Record_List * curr_record;
  	TSMD_CONTEXT * tsmd_context;

	curr_head=server_context.contexts_list.list.next;

	while(curr_head!=&server_context.contexts_list.list)
	{
		curr_record=(Record_List *)curr_head;
		curr_head=curr_head->next;
		tsmd_context=curr_record->record;
		if(tsmd_context==NULL)
			continue;
		ret=channel_inner_read(tsmd_context->tsmd_API_channel,tsmd_context->tsmd_send_buf,1024);
		if(ret>0)
		{
			int apino=*(int *)tsmd_context->tsmd_send_buf;
			int output_len=0;
			switch(apino)
			{
				case SUBTYPE(TSPI_IN,GETTCMOBJECT):
					output_len=proc_tspi_GetTcmObject(sub_proc,tsmd_context->tsmd_send_buf,
						tsmd_context->tsmd_recv_buf);	
					break;
				default:
					return -EINVAL;
			}
			ret=channel_inner_write(tsmd_context->tsmd_API_channel,tsmd_context->tsmd_recv_buf,output_len);
		}
	}
	return 0;
}

int proc_tspi_GetTcmObject(void * sub_proc,BYTE * in_buf,BYTE * out_buf)
{
	int ret;
	RECORD(TSPI_IN, GETTCMOBJECT) tspi_in;	
	RECORD(TSPI_OUT, GETTCMOBJECT) tspi_out;
        void * tspi_in_template = memdb_get_template(TYPE_PAIR(TSPI_IN,GETTCMOBJECT));
        if(tspi_in_template == NULL)
        {
                return -EINVAL;
        }
        void * tspi_out_template = memdb_get_template(TYPE_PAIR(TSPI_OUT,GETTCMOBJECT));
        if(tspi_out_template == NULL)
        {
                return -EINVAL;
        }

	ret=blob_2_struct(in_buf,&tspi_in,tspi_in_template);
	if(ret<0)
		return ret;
	tspi_out.returncode=0;
	tspi_out.paramSize=sizeof(tspi_out);
	tspi_out.hTCM=tspi_in.hContext;
	ret=struct_2_blob(&tspi_out,out_buf,tspi_out_template);
	return ret;
}
