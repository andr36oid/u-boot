// SPDX-License-Identifier: GPL-2.0-or-later
#include <common.h>
#include <malloc.h>
#include <linux/libfdt.h>
#include "rockchip_display.h"

#if defined(CONFIG_PLATFORM_ODROID_GOADV) && defined(CONFIG_SHA256)
#include <asm/io.h>
#include <clk.h>
#include <u-boot/sha256.h>
#include "rockchip_panel.h"
#include "a36-logo-proof.h"
#include "a36-logo-hardware.h"
#include "a36-logo-diag.h"
DECLARE_GLOBAL_DATA_PTR;
static struct display_state *staged;
static unsigned int generation,staged_generation;
static unsigned long staged_base,staged_size,staged_fb;
static unsigned int staged_width,staged_height,staged_bpp;
static void *staged_blob;
/* HELD diagnostics only. These records never authorize adoption. No new
 * hardware reads, writes, retries or storage I/O are performed here.
 */
static u8 handoff_diag[A36_DIAG_BYTES];
static unsigned int finalizer_calls;
static void diag_word(unsigned int word, u32 value)
{
 if (word < A36_D_CRC)
  a36_logo_put(handoff_diag, word, value);
}
void a36_logo_diag_panel(unsigned int word, int value)
{
 diag_word(word, (u32)value);
}
static void diag_reset(void)
{
 memset(handoff_diag, 0xff, sizeof(handoff_diag));
 diag_word(A36_D_MAGIC, A36_DIAG_MAGIC);
 diag_word(A36_D_VERSION, 1);
 diag_word(A36_D_LENGTH, sizeof(handoff_diag));
 diag_word(A36_D_STAGE, 1);
 diag_word(A36_D_REASON, A36_R_STAGED);
 diag_word(A36_D_ERRNO, 0);
 diag_word(A36_D_CALLS, finalizer_calls);
}
static void diag_commit(void *blob)
{
 int route;
 if (!blob || fdt_check_header(blob))return;
 route=fdt_path_offset(blob,A36_LOGO_ROUTE);
 if (route<0)return;
 a36_logo_put(handoff_diag,A36_D_CRC,
              a36_logo_crc(handoff_diag,A36_D_CRC*4));
 /* Staging reserves the complete property. Never grow the final FDT. */
 (void)fdt_setprop_inplace(blob,route,A36_DIAG_PROPERTY,
                         handoff_diag,sizeof(handoff_diag));
}
static void diag_finish(void *blob, unsigned int reason, int error)
{
 diag_word(A36_D_REASON,reason);diag_word(A36_D_ERRNO,(u32)error);
 diag_commit(blob);
}
#define DIAG_RETURN(reason, error) do { \
 diag_finish(blob,A36_R_##reason,error); return; \
} while (0)
static void blank_record(void *blob)
{
 u8 zero[A36_LOGO_BYTES]={0};int route;
 if(!blob || fdt_check_header(blob))return;
 route=fdt_path_offset(blob,A36_LOGO_ROUTE);
 if(route>=0)(void)fdt_setprop_inplace(blob,route,A36_LOGO_PROPERTY,zero,sizeof(zero));
}
void a36_logo_invalidate(void)
{
 generation++;staged=NULL;
 blank_record(staged_blob);
}
void a36_logo_clear(void *blob)
{
 int node=-1,depth=0,len;const void *p;
 a36_logo_invalidate();staged_blob=NULL;
 if(!blob || fdt_check_header(blob))return;
 /* Restart traversal after deletion because libfdt offsets can move. */
restart:
 node=-1;depth=0;
 while((node=fdt_next_node(blob,node,&depth))>=0) {
  p=fdt_getprop(blob,node,A36_LOGO_PROPERTY,&len);
  if(!p)p=fdt_getprop(blob,node,A36_DIAG_PROPERTY,&len);
  if(p && fdt_getprop(blob,node,A36_LOGO_PROPERTY,&len)){fdt_delprop(blob,node,A36_LOGO_PROPERTY);goto restart;}
  if(p){fdt_delprop(blob,node,A36_DIAG_PROPERTY);goto restart;}
 }
}
static int matching_contract(void *blob,u8 digest[32])
{
 u8 *bytes,*old;u8 loader_digest[32];int a,b,ret=-EINVAL;
 bytes=malloc(A36_LOGO_CONTRACT_MAX);old=malloc(A36_LOGO_CONTRACT_MAX);
 if(!bytes || !old){ret=-ENOMEM;goto out;}
 a=a36_logo_contract(gd->fdt_blob,old,A36_LOGO_CONTRACT_MAX);
 b=a36_logo_contract(blob,bytes,A36_LOGO_CONTRACT_MAX);
 if(a<=0 || b<=0 || a!=b || memcmp(old,bytes,a))goto out;
 sha256_csum_wd(old,a,loader_digest,CHUNKSZ_SHA256);
 sha256_csum_wd(bytes,b,digest,CHUNKSZ_SHA256);
 if(memcmp(loader_digest,digest,32))goto out;
 ret=0;
out:free(old);free(bytes);return ret;
}
void a36_logo_stage(struct display_state *s,void *blob,unsigned long base,
                    unsigned long size,unsigned long fb,unsigned int width,
                    unsigned int height,unsigned int bpp)
{
 int route,ret;u8 invalid[A36_LOGO_BYTES]={0},digest[32];u32 pitch;
 if(!s || !s->is_enable || !state_get_panel(s) || fb<base || fb-base>~0U ||
    width>4096 || height>4096 || (bpp!=16 && bpp!=24 && bpp!=32))return;
 if(fdt_node_check_compatible(gd->fdt_blob,0,"rockchip,rk3326"))return;
 pitch=ALIGN(width*bpp,32)/8;
 if(a36_logo_bounds(base,size,fb-base,pitch,width,height,bpp))return;
 ret=matching_contract(blob,digest);
 if(ret){printf("LOGO32: loader/kernel init-power contract differs; no proof\n");return;}
 route=fdt_path_offset(blob,A36_LOGO_ROUTE);if(route<0)return;
 /* Placeholder is invalid. It reserves space through FDT shrink/relocation;
  * only the last boot-jump hook can replace it with a completed proof.
  */
 if(fdt_increase_size(blob,A36_LOGO_BYTES+128))return;
 route=fdt_path_offset(blob,A36_LOGO_ROUTE);
 if(fdt_setprop(blob,route,A36_LOGO_PROPERTY,invalid,sizeof(invalid)))return;
 staged=s;staged_generation=generation;staged_blob=NULL; /* FDT may relocate. */
 staged_base=base;staged_size=size;staged_fb=fb;staged_width=width;
 staged_height=height;staged_bpp=bpp;
 /* Optional diagnostic allocation must not turn a successful proof stage
  * into a failure. The original token path above is unchanged.
  */
 diag_reset();
 if (!fdt_increase_size(blob,A36_DIAG_BYTES+64)) {
  route=fdt_path_offset(blob,A36_LOGO_ROUTE);
  if (route>=0 && !fdt_setprop(blob,route,A36_DIAG_PROPERTY,
                            handoff_diag,sizeof(handoff_diag)))
   diag_commit(blob);
 }
}
static int fresh_frame(void)
{
 ulong started=get_timer(0);u32 enabled=readl((void *)0xff460034)&1;
 u32 value;int ret=-ETIMEDOUT;
 diag_word(A36_D_VOP_ENABLE,enabled);
 value=readl((void *)0xff460018);diag_word(A36_D_VOP_SYS,value);
 if(value&2){diag_word(A36_D_REASON,A36_R_FRAME_STANDBY);return -EIO;}
 value=readl((void *)0xff460020);diag_word(A36_D_VOP_DSP,value);
 if(!(value&(1U<<24))){diag_word(A36_D_REASON,A36_R_FRAME_MIPI);return -EIO;}
 writel(0x00010001,(void *)0xff460038);
 writel(0x00010001,(void *)0xff460034);
 while(get_timer(started)<100) {
  value=readl((void *)0xff46003c);diag_word(A36_D_VOP_STATUS,value);
  if(value&1){ret=0;break;}
  udelay(100);
 }
 writel(0x00010000|enabled,(void *)0xff460034);
 if(ret)diag_word(A36_D_REASON,A36_R_FRAME_TIMEOUT);
 return ret;
}
void rockchip_display_finalize_handoff(void *blob)
{
 u8 record[A36_LOGO_BYTES]={0},digest[32],status=0;
 struct display_state *s=staged;struct rockchip_panel *panel;
 struct drm_display_mode *m;struct clk clk;ulong actual;
 u32 cmd,video,lp,pitch,value;int route,ret,i;u32 fields[10];
 staged=NULL;
 staged_blob=blob;blank_record(blob);
 if(a36_logo_get(handoff_diag,A36_D_MAGIC)!=A36_DIAG_MAGIC)diag_reset();
 diag_word(A36_D_STAGE,2);diag_word(A36_D_CALLS,++finalizer_calls);
 diag_word(A36_D_GENERATION,generation);
 diag_word(A36_D_STAGED_GENERATION,staged_generation);
 diag_finish(blob,A36_R_ENTERED,0);
 if(!s)DIAG_RETURN(NO_STAGE,-ENODATA);
 if(generation!=staged_generation)DIAG_RETURN(GENERATION,-ESTALE);
 diag_word(A36_D_DISPLAY_ENABLED,s->is_enable);
 if(!s->is_enable)DIAG_RETURN(DISPLAY_OFF,-EIO);
 if(!blob)DIAG_RETURN(BAD_FDT,-EINVAL);
 ret=fdt_check_header(blob);if(ret)DIAG_RETURN(BAD_FDT,ret);
 ret=matching_contract(blob,digest);if(ret)DIAG_RETURN(CONTRACT,ret);
 diag_word(A36_D_STAGE,3);
 panel=state_get_panel(s);if(!panel)DIAG_RETURN(NO_PANEL,-ENODEV);
 diag_word(A36_D_PANEL_INIT_ERROR,panel->init_error);
 if(panel->init_error)DIAG_RETURN(PANEL_INIT,panel->init_error);
 diag_word(A36_D_PANEL_PREPARED,panel->prepared_checked);
 if(!panel->prepared_checked)DIAG_RETURN(PANEL_PREPARED,-EIO);
 diag_word(A36_D_PANEL_ENABLED,panel->enabled_checked);
 if(!panel->enabled_checked)DIAG_RETURN(PANEL_ENABLED,-EIO);
 diag_word(A36_D_STAGE,4);
 m=&s->conn_state.mode;diag_word(A36_D_EXPECTED_DCLK,m->clock*1000U);
 if(m->clock<=0 || m->clock>300000)DIAG_RETURN(BAD_CLOCK,-EINVAL);
 ret=clk_get_by_name(s->crtc_state.dev,"dclk_vop",&clk);
 if(ret)DIAG_RETURN(CLOCK_LOOKUP,ret);
 actual=clk_get_rate(&clk);clk_free(&clk);
 diag_word(A36_D_ACTUAL_DCLK,actual);
 if(actual!=(ulong)m->clock*1000)DIAG_RETURN(CLOCK_RATE,-ERANGE);
 diag_word(A36_D_STAGE,5);
 value=readl((void *)0xff450004);diag_word(A36_D_DSI_POWER,value);
 if(value!=1)DIAG_RETURN(DSI_POWER,-EIO);
 value=readl((void *)0xff450034);diag_word(A36_D_DSI_MODE,value);
 if(value!=0)DIAG_RETURN(DSI_MODE,-EIO);
 value=readl((void *)0xff4500b0);diag_word(A36_D_DSI_PHY,value);
 if(!(value&1))DIAG_RETURN(DSI_PHY,-EIO);
 value=readl((void *)0xff45003c);diag_word(A36_D_DSI_PACKET,value);
 if((value&0x3fff)!=m->hdisplay)DIAG_RETURN(DSI_PACKET,-EIO);
 cmd=readl((void *)0xff450068);video=readl((void *)0xff450038);
 lp=readl((void *)0xff450094);
 diag_word(A36_D_DSI_CMD,cmd);diag_word(A36_D_DSI_VIDEO,video);
 diag_word(A36_D_DSI_LP,lp);diag_word(A36_D_STAGE,6);
 ret=rockchip_panel_loader_status(panel,&status);
 diag_word(A36_D_STATUS_RET,ret);diag_word(A36_D_DCS_STATUS,status);
 writel(cmd,(void *)0xff450068);writel(video,(void *)0xff450038);
 writel(lp,(void *)0xff450094);
 if(ret)DIAG_RETURN(PANEL_STATUS,ret);
 diag_word(A36_D_STAGE,7);
 ret=fresh_frame();
 if(ret){diag_finish(blob,a36_logo_get(handoff_diag,A36_D_REASON),ret);return;}
 diag_word(A36_D_STAGE,8);
 a36_logo_put(record,A36_L_MAGIC,A36_LOGO_MAGIC);
 a36_logo_put(record,A36_L_VERSION,A36_LOGO_VERSION);
 a36_logo_put(record,A36_L_LENGTH,sizeof(record));
 a36_logo_put(record,A36_L_PHASES,A36_LOGO_PHASES);
 a36_logo_put(record,A36_L_STATUS,status);
 a36_logo_put(record,A36_L_MASK,A36_LOGO_STATUS_MASK);
 memcpy(record+A36_L_DIGEST*4,digest,sizeof(digest));
 fields[0]=m->clock*1000U;fields[1]=m->hdisplay;fields[2]=m->hsync_start;
 fields[3]=m->hsync_end;fields[4]=m->htotal;fields[5]=m->vdisplay;
 fields[6]=m->vsync_start;fields[7]=m->vsync_end;fields[8]=m->vtotal;fields[9]=m->flags;
 for(i=0;i<10;i++)a36_logo_put(record,A36_L_CLOCK+i,fields[i]);
 a36_logo_put(record,A36_L_BASE_HI,((u64)staged_base)>>32);
 a36_logo_put(record,A36_L_BASE_LO,staged_base);
 a36_logo_put(record,A36_L_SIZE_HI,((u64)staged_size)>>32);
 a36_logo_put(record,A36_L_SIZE_LO,staged_size);
 a36_logo_put(record,A36_L_OFFSET,staged_fb-staged_base);
 pitch=ALIGN(staged_width*staged_bpp,32)/8;
 a36_logo_put(record,A36_L_PITCH,pitch);a36_logo_put(record,A36_L_WIDTH,staged_width);
 a36_logo_put(record,A36_L_HEIGHT,staged_height);a36_logo_put(record,A36_L_BPP,staged_bpp);
 for(i=0;i<A36_LOGO_HW_COUNT;i++)
  a36_logo_put(record,A36_L_HW+i,readl((void *)(ulong)a36_logo_registers[i].address)&a36_logo_registers[i].mask);
 a36_logo_put(record,A36_L_CRC,a36_logo_crc(record,A36_L_CRC*4));
 ret=a36_logo_wire_valid(record,sizeof(record));diag_word(A36_D_WIRE_RET,ret);
 if(ret)DIAG_RETURN(BAD_WIRE,ret);
 route=fdt_path_offset(blob,A36_LOGO_ROUTE);if(route<0)DIAG_RETURN(NO_ROUTE,route);
 ret=fdt_setprop_inplace(blob,route,A36_LOGO_PROPERTY,record,sizeof(record));
 diag_word(A36_D_WRITE_RET,ret);if(ret)DIAG_RETURN(WRITE_TOKEN,ret);
 diag_word(A36_D_STAGE,9);diag_finish(blob,A36_R_SUCCESS,0);
 printf("LOGO32: checked loader proof published at final handoff\n");
 staged=NULL;
}

#else
void a36_logo_diag_panel(unsigned int word,int value){}
void a36_logo_invalidate(void){}
void a36_logo_clear(void *blob){}
void a36_logo_stage(struct display_state *s,void *b,unsigned long base,
 unsigned long size,unsigned long fb,unsigned int w,unsigned int h,unsigned int bits){}
void rockchip_display_finalize_handoff(void *blob){}
#endif
