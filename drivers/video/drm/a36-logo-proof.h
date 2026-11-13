/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef A36_LOGO_PROOF_H
#define A36_LOGO_PROOF_H
#ifdef __KERNEL__
#include <linux/libfdt.h>
#include <linux/string.h>
#endif
/* Wire format: 80 big-endian u32 cells. No native structs or DT phandles.
 * This is boot-chain transaction evidence, NOT an authentication mechanism.
 * Environment supplies libfdt, u8/u32/u64, memcmp/memcpy and SHA-256.
 */
#define A36_LOGO_PROPERTY "andr36oid,loader-panel-proof"
#define A36_LOGO_WORDS 80
#define A36_LOGO_BYTES (A36_LOGO_WORDS * 4)
#define A36_LOGO_MAGIC 0x41334c50U
#define A36_LOGO_VERSION 1U
#define A36_LOGO_PHASES 0x7fU
#define A36_LOGO_STATUS_MASK 0x1cU
#define A36_LOGO_CONTRACT_MAX 16384
#define A36_LOGO_PANEL "/dsi@ff450000/panel@0"
#define A36_LOGO_ROUTE "/display-subsystem/route/route-dsi"
enum a36_logo_word {
 A36_L_MAGIC=0, A36_L_VERSION, A36_L_LENGTH, A36_L_PHASES,
 A36_L_STATUS, A36_L_MASK, A36_L_RESERVED0, A36_L_RESERVED1,
 A36_L_DIGEST=8, /* 8 words */
 A36_L_CLOCK=16, A36_L_HD, A36_L_HSS, A36_L_HSE, A36_L_HT,
 A36_L_VD, A36_L_VSS, A36_L_VSE, A36_L_VT, A36_L_FLAGS,
 A36_L_BASE_HI=26, A36_L_BASE_LO, A36_L_SIZE_HI, A36_L_SIZE_LO,
 A36_L_OFFSET, A36_L_PITCH, A36_L_WIDTH, A36_L_HEIGHT, A36_L_BPP,
 A36_L_MIRROR, A36_L_HW=36, /* 39 audited hardware words */
 A36_L_RESERVED2=75, A36_L_CRC=79
};
struct a36_logo_contract {
 u8 *bytes;
 unsigned int used, size;
 int error;
};
static inline u32 a36_logo_get(const void *p, unsigned int word)
{
 const u8 *b = (const u8 *)p + word * 4;
 return ((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|b[3];
}
static inline void a36_logo_put(void *p, unsigned int word, u32 v)
{
 u8 *b = (u8 *)p + word * 4;
 b[0]=v>>24; b[1]=v>>16; b[2]=v>>8; b[3]=v;
}
static inline u32 a36_logo_crc(const void *p, unsigned int length)
{
 const u8 *b=p;
 u32 crc=~0U;
 unsigned int i, bit;
 for (i=0;i<length;i++) {
  crc^=b[i];
  for (bit=0;bit<8;bit++) crc=(crc>>1)^(0xedb88320U & (0U-(crc&1)));
 }
 return ~crc;
}
static inline int a36_logo_bounds(u64 base, u64 size, u32 offset, u32 pitch,
                                  u32 width, u32 height, u32 bpp)
{
 u64 bytes=(u64)pitch*height;
 if (!base || !size || base+size<base || !width || !height ||
     width>4096 || height>4096 || (bpp!=16 && bpp!=24 && bpp!=32) ||
     pitch<(u64)width*(bpp/8) || (pitch&3) ||
     offset>=size || bytes>size-offset) return -1;
 return 0;
}
static inline int a36_logo_wire_valid(const void *p, unsigned int len)
{
 u64 base,size;
 unsigned int i;
 if (!p || len!=A36_LOGO_BYTES ||
     a36_logo_get(p,A36_L_MAGIC)!=A36_LOGO_MAGIC ||
     a36_logo_get(p,A36_L_VERSION)!=A36_LOGO_VERSION ||
     a36_logo_get(p,A36_L_LENGTH)!=A36_LOGO_BYTES ||
     a36_logo_get(p,A36_L_PHASES)!=A36_LOGO_PHASES ||
     a36_logo_get(p,A36_L_MASK)!=A36_LOGO_STATUS_MASK ||
     (a36_logo_get(p,A36_L_STATUS)&A36_LOGO_STATUS_MASK)!=A36_LOGO_STATUS_MASK ||
     a36_logo_get(p,A36_L_STATUS)>255 ||
     a36_logo_get(p,A36_L_RESERVED0)||a36_logo_get(p,A36_L_RESERVED1) ||
     a36_logo_get(p,A36_L_MIRROR)>1 ||
     a36_logo_get(p,A36_L_CRC)!=a36_logo_crc(p,A36_L_CRC*4)) return -1;
 for(i=A36_L_RESERVED2;i<A36_L_CRC;i++) if(a36_logo_get(p,i)) return -1;
 if (!a36_logo_get(p,A36_L_CLOCK) || a36_logo_get(p,A36_L_CLOCK)>300000000 ||
     !a36_logo_get(p,A36_L_HD) || !a36_logo_get(p,A36_L_VD) ||
     a36_logo_get(p,A36_L_HD)>=a36_logo_get(p,A36_L_HSS) ||
     a36_logo_get(p,A36_L_HSS)>=a36_logo_get(p,A36_L_HSE) ||
     a36_logo_get(p,A36_L_HSE)>=a36_logo_get(p,A36_L_HT) ||
     a36_logo_get(p,A36_L_VD)>=a36_logo_get(p,A36_L_VSS) ||
     a36_logo_get(p,A36_L_VSS)>=a36_logo_get(p,A36_L_VSE) ||
     a36_logo_get(p,A36_L_VSE)>=a36_logo_get(p,A36_L_VT)) return -1;
 base=((u64)a36_logo_get(p,A36_L_BASE_HI)<<32)|a36_logo_get(p,A36_L_BASE_LO);
 size=((u64)a36_logo_get(p,A36_L_SIZE_HI)<<32)|a36_logo_get(p,A36_L_SIZE_LO);
 return a36_logo_bounds(base,size,a36_logo_get(p,A36_L_OFFSET),
       a36_logo_get(p,A36_L_PITCH),a36_logo_get(p,A36_L_WIDTH),
       a36_logo_get(p,A36_L_HEIGHT),a36_logo_get(p,A36_L_BPP));
}
static __attribute__((__noinline__)) void a36_contract_bytes(struct a36_logo_contract *c,
                                     const void *p, unsigned int len)
{
 if(c->error)return;
 if(len>c->size-c->used){c->error=-1;return;}
 memcpy(c->bytes+c->used,p,len);c->used+=len;
}
static __attribute__((__noinline__)) void a36_contract_u32(struct a36_logo_contract *c,u32 v)
{
 u8 b[4];a36_logo_put(b,0,v);a36_contract_bytes(c,b,4);
}
static __attribute__((__noinline__)) void a36_contract_property(struct a36_logo_contract *c,
                                        const void *fdt,int node,const char *key)
{
 int len;const void *p=fdt_getprop(fdt,node,key,&len);
 /* Missing is explicitly distinguished from an empty or zero property. */
 if(!p){a36_contract_u32(c,~0U);return;}
 if(len<0 || len>A36_LOGO_CONTRACT_MAX){c->error=-1;return;}
 a36_contract_u32(c,len);a36_contract_bytes(c,p,len);
}
static __attribute__((__noinline__)) void a36_contract_identity(struct a36_logo_contract *c,
                                        const void *fdt,int node)
{
 char path[256];int len;
 if(node<0 || fdt_get_path(fdt,node,path,sizeof(path))){c->error=-1;return;}
 len=strlen(path)+1;a36_contract_u32(c,len);a36_contract_bytes(c,path,len);
 a36_contract_property(c,fdt,node,"reg");
}
static __attribute__((__noinline__)) int a36_contract_phandle(const void *fdt,int node,const char *key)
{
 int len;const fdt32_t *p=fdt_getprop(fdt,node,key,&len);
 if(!p)return -FDT_ERR_NOTFOUND;
 if(len!=4)return -FDT_ERR_BADVALUE;
 return fdt_node_offset_by_phandle(fdt,fdt32_to_cpu(*p));
}
static __attribute__((__noinline__)) void a36_contract_gpio(struct a36_logo_contract *c,
                                    const void *fdt,int node,const char *key)
{
 int len,n;const fdt32_t *p=fdt_getprop(fdt,node,key,&len);
 if(!p){a36_contract_u32(c,~0U);return;}
 if(len!=12){c->error=-1;return;}
 n=fdt_node_offset_by_phandle(fdt,fdt32_to_cpu(p[0]));
 a36_contract_identity(c,fdt,n);
 a36_contract_bytes(c,p+1,8);
}
static __attribute__((__noinline__)) void a36_contract_supply(struct a36_logo_contract *c,
                                      const void *fdt,int node,unsigned int depth)
{
 static const char * const keys[]={"regulator-name","regulator-min-microvolt",
 "regulator-max-microvolt","regulator-boot-on","regulator-always-on",
 "enable-active-high","startup-delay-us","off-on-delay-us"};
 unsigned int i;int parent;
 for(;depth<=8;depth++) {
  if(node==-FDT_ERR_NOTFOUND){a36_contract_u32(c,~0U);return;}
  if(node<0){c->error=-1;return;}
  a36_contract_identity(c,fdt,node);
  for(i=0;i<sizeof(keys)/sizeof(keys[0]);i++)
   a36_contract_property(c,fdt,node,keys[i]);
  a36_contract_gpio(c,fdt,node,"gpio");
  a36_contract_gpio(c,fdt,node,"gpios");
  parent=a36_contract_phandle(fdt,node,"vin-supply");
  node=parent;
 }
 c->error=-1;
}
static __attribute__((__noinline__)) void a36_contract_pinctrl(struct a36_logo_contract *c,
                                       const void *fdt,int node)
{
 static const char * const config_keys[]={"bias-disable","bias-pull-up",
 "bias-pull-down","output-high","output-low","drive-strength"};
 int len,n,clen,i,j,k;const fdt32_t *groups,*pins;
 groups=fdt_getprop(fdt,node,"pinctrl-0",&len);
 if(!groups){a36_contract_u32(c,~0U);return;}
 if(len<0 || len>64 || (len&3)){c->error=-1;return;}
 a36_contract_u32(c,len/4);
 for(i=0;i<len/4;i++){
  n=fdt_node_offset_by_phandle(fdt,fdt32_to_cpu(groups[i]));
  pins=fdt_getprop(fdt,n,"rockchip,pins",&clen);
  if(!pins || clen<=0 || (clen%16) || clen>1024){c->error=-1;return;}
  a36_contract_u32(c,clen/16);
  for(j=0;j<clen/4;j+=4){
   a36_contract_bytes(c,pins+j,12);
   n=fdt_node_offset_by_phandle(fdt,fdt32_to_cpu(pins[j+3]));
   if(n<0){c->error=-1;return;}
   for(k=0;k<sizeof(config_keys)/sizeof(config_keys[0]);k++)
    a36_contract_property(c,fdt,n,config_keys[k]);
  }
 }
}
/* Canonical panel/link/power/backlight contract. Whitelist and field order are
 * versioned. Values are resolved to paths/addresses, never numeric phandles.
 * A stricter missing/default distinction can reject an otherwise equivalent
 * board; that conservatively selects checked cold init, never blind adoption.
 */
static __attribute__((__noinline__)) int a36_logo_contract(const void *fdt,u8 *bytes,unsigned int size)
{
 static const char * const panel_keys[]={"compatible","reg","dsi,lanes",
 "dsi,format","dsi,flags","panel-init-sequence","panel-exit-sequence",
 "prepare-delay-ms","reset-delay-ms","init-delay-ms","enable-delay-ms",
 "disable-delay-ms","unprepare-delay-ms","power-invert"};
 static const char * const bl_keys[]={"compatible","brightness-levels",
 "default-brightness-level","post-pwm-on-delay-ms","pwm-off-delay-ms"};
 struct a36_logo_contract c={bytes,0,size,0};
 int panel,bl,len,n;unsigned int i;const fdt32_t *p;
 panel=fdt_path_offset(fdt,A36_LOGO_PANEL);
 if(panel<0)return -1;
 a36_contract_u32(&c,A36_LOGO_VERSION);
 for(i=0;i<sizeof(panel_keys)/sizeof(panel_keys[0]);i++)
  a36_contract_property(&c,fdt,panel,panel_keys[i]);
 a36_contract_gpio(&c,fdt,panel,"reset-gpios");
 a36_contract_gpio(&c,fdt,panel,"enable-gpios");
 a36_contract_pinctrl(&c,fdt,panel);
 a36_contract_supply(&c,fdt,a36_contract_phandle(fdt,panel,"power-supply"),0);
 bl=a36_contract_phandle(fdt,panel,"backlight");
 if(bl<0)return -1;
 a36_contract_identity(&c,fdt,bl);
 for(i=0;i<sizeof(bl_keys)/sizeof(bl_keys[0]);i++)
  a36_contract_property(&c,fdt,bl,bl_keys[i]);
 a36_contract_gpio(&c,fdt,bl,"enable-gpios");
 a36_contract_supply(&c,fdt,a36_contract_phandle(fdt,bl,"power-supply"),0);
 a36_contract_pinctrl(&c,fdt,bl);
 p=fdt_getprop(fdt,bl,"pwms",&len);
 if(!p || len!=16)return -1;
 n=fdt_node_offset_by_phandle(fdt,fdt32_to_cpu(p[0]));
 a36_contract_identity(&c,fdt,n);a36_contract_bytes(&c,p+1,12);
 a36_contract_pinctrl(&c,fdt,n);
 return c.error ? -1 : (int)c.used;
}
#endif
