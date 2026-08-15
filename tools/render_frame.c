/* Renders one live-game frame from the recompilation to a PPM so it can be
 * diffed against the reference runner's --dump-frame output:
 *
 *   make render-frame && ./build/render_frame 1200 ours.ppm
 *   ./build/battle_squadron_native --frames 5200 --autofire \
 *       --video-from 5100 --dump-frame ref.ppm
 *
 * It stops on the first frame past 700 holding a compact cluster of the
 * gold sprite colours, which is the artefact under investigation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "ocs_video.h"

static uint8_t rd(void *user, uint32_t a){ return bs_recomp_read8(user, a); }

static int display_frame(BsRecomp *m, uint8_t *clean){
    for(int g=0;g<512;g++){ if(bs_recomp_run(m,1)!=BS_RECOMP_OK) return 0;
        if(m->cpu.pc==0xb54) break; }
    memcpy(clean, m->memory + 0x62000, 0x1e000);
    for(int g=0;g<512;g++){ if(bs_recomp_run(m,1)!=BS_RECOMP_OK) return 0;
        if(m->cpu.pc==0xaa0) return 1; }
    return 0;
}

int main(int argc, char **argv){
    long frames = argc>1 ? atol(argv[1]) : 2000;
    const char *out = argc>2 ? argv[2] : "ours.ppm";
    BsRecomp *m=calloc(1,sizeof *m);
    uint8_t *clean=malloc(0x1e000);
    if(bs_recomp_init(m,"original/whdload/BattleSquadron/data")!=BS_RECOMP_OK) return 1;
    bs_recomp_run(m,20000);
    bs_recomp_set_input(m,0,BS_INPUT_FIRE);
    for(long g=0;g<20000&&m->cpu.pc!=0xd52;g++) if(bs_recomp_run(m,1)!=BS_RECOMP_OK) return 1;
    bs_recomp_run(m,14);
    bs_recomp_set_input(m,0,0);
    bs_recomp_enable_live_input(m,1);

    OcsVideo *video=ocs_video_create();
    int found=0;
    for(long f=0; f<frames && !found; f++){
        bs_recomp_set_input(m,0,BS_INPUT_FIRE);
        if(!display_frame(m,clean)){ printf("stop: %s\n",m->error); break; }
        /* Stop on a frame whose rendered image contains the gold sprite. */
        {
            OcsVideoSource s0={ .user=m, .read8=rd,
                .chip_mask=BS_RECOMP_MEMORY_SIZE-1,
                .copper_address=((uint32_t)m->custom[0x080>>1]<<16)|
                                 m->custom[0x082>>1] };
            const uint32_t *p0=ocs_video_render(video,&s0);
            int gold=0, minx=999,maxx=-1,miny=999,maxy=-1;
            for(int i=0;i<OCS_VIDEO_WIDTH*OCS_VIDEO_HEIGHT;i++){
                unsigned R=p0[i]&0xff,G=(p0[i]>>8)&0xff,B=(p0[i]>>16)&0xff;
                if(R>150&&B<110&&G<R){ gold++;
                    int x=i%OCS_VIDEO_WIDTH,y=i/OCS_VIDEO_WIDTH;
                    if(x<minx) minx=x;
                    if(x>maxx) maxx=x;
                    if(y<miny) miny=y;
                    if(y>maxy) maxy=y;
                }
            }
            if(gold>=20 && gold<=260 && f>=700 && maxx-minx<90 && maxy-miny<90){ found=1;
                printf("gold pixels=%d bbox x %d..%d y %d..%d on frame %ld\n",
                       gold,minx,maxx,miny,maxy,f); }
        }
        if(!found) memcpy(m->memory + 0x62000, clean, 0x1e000);
    }
    if(!found){ printf("no visible type-$07 found\n"); return 3; }
    OcsVideoSource src={ .user=m, .read8=rd,
        .chip_mask=BS_RECOMP_MEMORY_SIZE-1,
        .copper_address=((uint32_t)m->custom[0x080>>1]<<16)|m->custom[0x082>>1] };
    const uint32_t *px=ocs_video_render(video,&src);
    FILE *fp=fopen(out,"wb");
    fprintf(fp,"P6\n%d %d\n255\n",OCS_VIDEO_WIDTH,OCS_VIDEO_HEIGHT);
    for(int i=0;i<OCS_VIDEO_WIDTH*OCS_VIDEO_HEIGHT;i++){
        uint32_t v=px[i]; fputc(v&0xff,fp); fputc((v>>8)&0xff,fp); fputc((v>>16)&0xff,fp);
    }
    fclose(fp);
    printf("wrote %s after %ld frames\n",out,frames);
    for(int s=0;s<12;s++){unsigned r=0x2dc80+s*0x50;
        if(!bs_recomp_read16(m,r)) continue;
        printf("  hos %2d x=%4u y=%4u type=$%02X f63=%3u gfx36=$%06X h50=%u w52=%u\n",
            s,bs_recomp_read16(m,r),bs_recomp_read16(m,r+4),bs_recomp_read8(m,r+31),
            bs_recomp_read8(m,r+63),bs_recomp_read32(m,r+36),
            bs_recomp_read16(m,r+50),bs_recomp_read16(m,r+52));}
    printf("  scroll7204=%u\n", bs_recomp_read16(m, 0x8000+7204));
    return 0;
}
