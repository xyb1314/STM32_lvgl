#include "wav.h"
#include "i2s.h"
#include "delay.h"
#include "wm8978.h"
#include "malloc.h"
#include "fatfs.h"
#include "stdio.h"

#if SYSTEM_SUPPORT_OS
#include "includes.h"
#define WAV_TRANSFER_END_FLAG 1

OS_FLAG_GRP wavtransferend_flag;		//事件标志组		
#else
static __IO uint8_t wavtransferend = 0;	//i2s传输完成标志
#endif

static __IO uint8_t wavwitchbuf = 0;		//i2sbufx指示标志

static audiodev_t audiodev;

static audio_msg_show_cb_t audio_msg_show_cb = NULL;		//信息显示回调函数
static wav_get_target_curtime_cb_t wav_get_target_curtime_cb = NULL;	//获取目标音乐时间点回调函数(调用时都需设置一次)

#if SYSTEM_SUPPORT_OS

//I2S传输完成事件标志组初始化
void wav_flag_init(void)
{
	OS_ERR err;
	OSFlagCreate(&wavtransferend_flag, "touch flags", WAV_TRANSFER_END_FLAG, &err);		//创建事件标志组
}

#endif

// WAV解析初始化
// fname:文件路径+文件名
// wavx:wav 信息存放结构体指针
//返回值:0,成功;1,打开文件失败;2,非WAV文件;3,DATA区域未找到.
uint8_t wav_decode_init(const TCHAR *fname, wavctrl_t *wavx)
{
	FIL *ftemp;
	uint8_t *buf;
	uint32_t br = 0;
	uint8_t res = 0;

	ChunkRIFF *riff;
	ChunkFMT *fmt;
	ChunkFACT *fact;
	ChunkDATA *data;
	ftemp = (FIL *)mymalloc(SRAMEX, sizeof(FIL));
	buf = mymalloc(SRAMEX, 512);
	if (ftemp && buf) //内存申请成功
	{
		res = f_open(ftemp, (TCHAR *)fname, FA_READ); //打开文件
		if (res == FR_OK)
		{
			f_read(ftemp, buf, 512, &br);		//读取512字节在数据
			riff = (ChunkRIFF *)buf;				//获取RIFF块
			if (riff->Format == 0X45564157) //是WAV文件
			{
				fmt = (ChunkFMT *)(buf + 12);												 //获取FMT块
				fact = (ChunkFACT *)(buf + 12 + 8 + fmt->ChunkSize); //读取FACT块
				if (fact->ChunkID == 0X74636166 || fact->ChunkID == 0X5453494C)
					wavx->datastart = 12 + 8 + fmt->ChunkSize + 8 + fact->ChunkSize; //具有fact/LIST块的时候(未测试)
				else
					wavx->datastart = 12 + 8 + fmt->ChunkSize;
				data = (ChunkDATA *)(buf + wavx->datastart); //读取DATA块
				if (data->ChunkID == 0X61746164)						 //解析成功!
				{
					wavx->audioformat = fmt->AudioFormat; //音频格式
					wavx->nchannels = fmt->NumOfChannels; //通道数
					wavx->samplerate = fmt->SampleRate;		//采样率
					wavx->bitrate = fmt->ByteRate * 8;		//得到位速
					wavx->blockalign = fmt->BlockAlign;		//块对齐
					wavx->bps = fmt->BitsPerSample;				//位数,16/24/32位

					wavx->datasize = data->ChunkSize;			 //数据块大小
					wavx->datastart = wavx->datastart + 8; //数据流开始的地方.

					printf("wavx->audioformat:%d\r\n", wavx->audioformat);
					printf("wavx->nchannels:%d\r\n", wavx->nchannels);
					printf("wavx->samplerate:%d\r\n", wavx->samplerate);
					printf("wavx->bitrate:%d\r\n", wavx->bitrate);
					printf("wavx->blockalign:%d\r\n", wavx->blockalign);
					printf("wavx->bps:%d\r\n", wavx->bps);
					printf("wavx->datasize:%d\r\n", wavx->datasize);
					printf("wavx->datastart:%d\r\n", wavx->datastart);
				}
				else
					res = 3; // data区域未找到.
			}
			else
				res = 2; //非wav文件
		}
		else
			res = 1; //打开文件错误
	}
	f_close(ftemp);
	myfree(SRAMEX, ftemp); //释放内存
	myfree(SRAMEX, buf);
	return 0;
}

//判断文件是不是.wav文件
bool wav_is_legal(const char *name)
{
	uint32_t i = 0;
	const char *attr = NULL;
	
	while(1)
	{
		if(name[i] == '\0')		
			break;
		else if(i > _MAX_LFN)
			return false;
		
		i++;
	}	//计算文件名长度
	
	for(uint8_t j = 0; j < 5; j++)
	{
		if(name[--i] == '.')
		{
			attr = &name[i];	//找到后缀名
			break;
		}
	}
	
	return !(strcmp(attr,".wav") && strcmp(attr,".WAV"));
}


//得到当前播放时间
//fx:文件指针
//wavx:wav播放控制器
void wav_get_curtime(FIL*fx, wavctrl_t *wavx)
{
	long long fpos;  
	
 	wavx->totsec=wavx->datasize / (wavx->bitrate / 8);	//歌曲总长度(单位:秒) 
	fpos=fx->fptr - wavx->datastart; 					//得到当前文件播放到的地方 
	wavx->cursec=fpos * wavx->totsec/wavx->datasize;	//当前播放到第多少秒了?	
}

//设置当前播放时间点
//offst: 相对于0的偏移时间点
void wav_set_curtime(FIL *fx, wavctrl_t *wavx, uint32_t sec_offst)
{
	long long fpos = 0; 
	
	if(sec_offst > wavx->totsec)
		return;
	
	fpos += sec_offst * (wavx->bitrate / 8);		//偏移秒 * 每秒数据量 = 数据偏移量
	
	f_lseek(fx, wavx->datastart + fpos);		//设置文件偏移
}

//开始音频播放
void audio_start(uint8_t* buf0,uint8_t *buf1,uint16_t num)
{
	audiodev.status = 0x03;//开始播放+非暂停
	I2S_Play_Start(buf0, buf1, num);
} 

bool audio_is_play(void)
{
	return !!(audiodev.status & 0x01);
}

//继续音频播放
void audio_continue(void)
{
	audiodev.status |= (1 << 0);
} 

//暂停音频播放
void audio_pause(void)
{
	audiodev.status &= ~(1 << 0);
}

//停止音频播放
void audio_stop(void)
{
	audiodev.status = 0;
	I2S_Play_Stop();
}  

//WAV播放时,I2S DMA传输回调函数
static void wav_i2s_dma_tx_callback(void) 
{   
	uint16_t i;
	if(DMA1_Stream4->CR&(1<<19))
	{
		wavwitchbuf=0;
		if((audiodev.status&0X01)==0)
		{
			for(i=0;i<WAV_I2S_TX_DMA_BUFSIZE;i++)//暂停
			{
				audiodev.i2sbuf1[i]=0;//填充0
			}
		}
	}
	else 
	{
		wavwitchbuf=1;
		if((audiodev.status&0X01)==0)
		{
			for(i=0;i<WAV_I2S_TX_DMA_BUFSIZE;i++)//暂停
			{
				audiodev.i2sbuf2[i]=0;//填充0
			}
		}
	}
#if SYSTEM_SUPPORT_OS
	OS_ERR err;
	OSFlagPost(&wavtransferend_flag, WAV_TRANSFER_END_FLAG, OS_OPT_POST_FLAG_SET | OS_OPT_POST_NO_SCHED, &err);		//向事件组发送标志位 
#else
	wavtransferend=1;
#endif
} 

//填充buf
//buf:数据区
//size:填充数据量
//bits:位数(16/24)
//返回值:读到的数据个数
static uint32_t wav_buffill(audiodev_t *dev, uint8_t *buf, uint16_t size, uint8_t bits)
{
	uint16_t readlen=0;
	uint32_t bread;
	uint16_t i;
	uint8_t *p;
	if(bits==24)//24bit音频,需要处理一下
	{
		readlen=(size/4)*3;							//此次要读取的字节数
		f_read(dev->file,dev->tbuf,readlen,(UINT*)&bread);	//读取数据
		p=dev->tbuf;
		for(i=0;i<size;)
		{
			buf[i++]=p[1];
			buf[i]=p[2]; 
			i+=2;
			buf[i++]=p[0];
			p+=3;
		} 
		bread=(bread*4)/3;		//填充后的大小.
	}else 
	{
		f_read(dev->file,buf,size,(UINT*)&bread);//16bit音频,直接读取数据  
		if(bread<size)//不够数据了,补充0
		{
			for(i=bread;i<size-bread;i++)buf[i]=0; 
		}
	}
	return bread;
}  

//显示播放时间,比特率 信息  
//totsec;音频文件总时间长度
//cursec:当前播放时间
//bitrate:比特率(位速)
void audio_msg_show(uint32_t totsec,uint32_t cursec,uint32_t bitrate)
{	
	static uint32_t playtime=0XFFFF;//播放时间标记	      
	if(playtime!=cursec)					//需要更新显示时间
	{
		playtime=cursec;
		printf("playtime:%d:%d / %d:%d\r\n", playtime/60, playtime%60, totsec/60, totsec%60);
		
		if(audio_msg_show_cb != NULL)
			audio_msg_show_cb(totsec, playtime);		//调用外部信息显示回调
	} 		 
}

//注册回调函数
void audio_set_cb(void *cb, Audio_Callback index)
{
	switch(index)
	{
		case Audio_Callback_Msg_Show:
			audio_msg_show_cb = (audio_msg_show_cb_t)cb;
			break;
		case Audio_Callback_Get_Target_Curtime:
			wav_get_target_curtime_cb = (wav_get_target_curtime_cb_t)cb;
			break;
		default:;
	}
}

//播放某个WAV文件
//fname:wav文件路径.
//返回值:
//其他:错误
uint8_t wav_play_song(const TCHAR *fname, wavctrl_t *wavctrl)
{
	uint8_t res;  
	uint32_t fillnum; 
	audiodev.file=(FIL*)mymalloc(SRAMIN,sizeof(FIL));
	audiodev.i2sbuf1=mymalloc(SRAMIN,WAV_I2S_TX_DMA_BUFSIZE);
	audiodev.i2sbuf2=mymalloc(SRAMIN,WAV_I2S_TX_DMA_BUFSIZE);
	audiodev.tbuf=mymalloc(SRAMIN,WAV_I2S_TX_DMA_BUFSIZE);
	if(audiodev.file&&audiodev.i2sbuf1&&audiodev.i2sbuf2&&audiodev.tbuf)
	{ 
			if(wavctrl->bps==16)
			{
				WM8978_I2S_Cfg(2,0);	//飞利浦标准,16位数据长度
				I2S2_Init(I2S_STANDARD_PHILIPS,I2S_MODE_MASTER_TX,I2S_CPOL_LOW,I2S_DATAFORMAT_16B_EXTENDED);	//飞利浦标准,主机发送,时钟低电平有效,16位扩展帧长度
			}else if(wavctrl->bps==24)
			{
				WM8978_I2S_Cfg(2,2);	//飞利浦标准,24位数据长度
				I2S2_Init(I2S_STANDARD_PHILIPS,I2S_MODE_MASTER_TX,I2S_CPOL_LOW,I2S_DATAFORMAT_24B);	//飞利浦标准,主机发送,时钟低电平有效,24位长度
			}
			I2S2_SampleRate_Set(wavctrl->samplerate);//设置采样率
			I2S_SetCb(wav_i2s_dma_tx_callback);			//回调函数指wav_i2s_dma_callback
			audio_stop();
			res=f_open(audiodev.file,(TCHAR*)fname,FA_READ);	//打开文件
			if(res==0) 
			{
				f_lseek(audiodev.file, wavctrl->datastart);		//跳过文件头
				fillnum=wav_buffill(&audiodev, audiodev.i2sbuf1,WAV_I2S_TX_DMA_BUFSIZE,wavctrl->bps);
				fillnum=wav_buffill(&audiodev, audiodev.i2sbuf2,WAV_I2S_TX_DMA_BUFSIZE,wavctrl->bps);
				audio_start(audiodev.i2sbuf1,audiodev.i2sbuf2,WAV_I2S_TX_DMA_BUFSIZE/2);  
				while(res==0)
				{ 
#if SYSTEM_SUPPORT_OS
					OS_ERR err;
					OSFlagPend(&wavtransferend_flag, WAV_TRANSFER_END_FLAG, 0, OS_OPT_PEND_FLAG_SET_ALL | OS_OPT_PEND_FLAG_CONSUME, NULL, &err);//阻塞等待标志组
#else
					while(wavtransferend==0);//等待wav传输完成; 
					wavtransferend=0;
#endif
					if(fillnum!=WAV_I2S_TX_DMA_BUFSIZE)//播放结束
					{
						break;
					} 
 					if(wavwitchbuf)
						fillnum=wav_buffill(&audiodev, audiodev.i2sbuf2,WAV_I2S_TX_DMA_BUFSIZE,wavctrl->bps);//填充buf2
					else 
						fillnum=wav_buffill(&audiodev, audiodev.i2sbuf1,WAV_I2S_TX_DMA_BUFSIZE,wavctrl->bps);//填充buf1
					while(1)
					{
						wav_get_curtime(audiodev.file, wavctrl);//得到总时间和当前播放的时间 
						audio_msg_show(wavctrl->totsec,wavctrl->cursec,wavctrl->bitrate);
						
						if(wav_get_target_curtime_cb != NULL)		//获取音乐目标时间点回调函数被设置，需要调整音乐播放时间点
						{
							uint32_t offset = wav_get_target_curtime_cb();
							wav_set_curtime(audiodev.file, wavctrl, offset);		//设置音频播放时间点
							wav_get_target_curtime_cb = NULL;		//回调函数清除
						}
						
						if((audiodev.status & 0X02) == 0)	//结束
						{
							res = 1;
							break;
						}
						else if((audiodev.status & 0X01) == 0)		//暂停
							delay_ms(50);
						else
							break;
					}
				}
				audio_stop(); 
			}else res=0XFF; 
		}else res=0XFF;
	f_close(audiodev.file);
	myfree(SRAMIN,audiodev.tbuf);	//释放内存
	myfree(SRAMIN,audiodev.i2sbuf1);//释放内存
	myfree(SRAMIN,audiodev.i2sbuf2);//释放内存 
	myfree(SRAMIN,audiodev.file);	//释放内存 
	return res;
}



