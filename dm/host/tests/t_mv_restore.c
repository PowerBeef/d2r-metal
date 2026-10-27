// 位置守卫的写回（dm_cursor.c mv_offset / mv_restore）：写回时控件 x 不是我们上次写的值 = 原版暂时把它挪走了，不能就此丢掉条目，要等 x 回到我们的值再写回原位；
// 丢掉的话我们的位移被原版当原位，下次再叠一次。出处是原先的储藏箱加宽（＋ 关储藏箱时原版同一帧把物品栏单开挪走，实跑 219 → 446 → 673 → 900 → 1127）；
// 加宽删掉之后用魔盒并排那一位（MV_CUBE，名单里有物品栏面板）照样验这条规则
// Position guard restore (dm_cursor.c mv_offset / mv_restore): if the widget x is not the value we last wrote when restoring, the original game has moved it away for now; the entry must not be dropped,
// but kept until x holds our value again, then the original is restored; dropping it would let the original game adopt our shift as its origin and stack it again next time. The rule comes from the former
// stash widening (closing the stash with + made the original game move the inventory to its stand-alone position in the same frame; real run 219 -> 446 -> 673 -> 900 -> 1127);
// with the widening gone it is checked through the cube side-by-side bit (MV_CUBE, whose list includes the inventory panel)
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
uint64_t dm_guest_frame(void) { return 100; }
uint64_t now_ns(void) { return 0; }
int dm_padmode_busy(void) { return 0; }
int dm_uiscreen_ingame(void) { return 1; }
void dm_ui_rect(int *x,int *y,int *w,int *h) { *x=0; *y=0; *w=2360; *h=1326; }
void dm_uiscreen_set_ingame(int on) { (void)on; }
void dm_custom_on_uimsg(const char *cat,const char *act,const char *arg) { (void)cat; (void)act; (void)arg; }
static int checks,fails;
#define CHECK(c) do { checks++; if(!(c)) { fails++; printf("FAIL line %d: %s\n",__LINE__,#c); } } while(0)
static _Alignas(8) unsigned char root[160], inv[160];
static uint64_t children[1];
static uint64_t ptr(void *p) { return (uint64_t)(uintptr_t)p; }
static int32_t *x(void) { return (int32_t *)(inv+0x70); }
enum { HOME = -8, SHIFT = 227, ALONE = -816 };   // 当时实跑的值：原位、位移、原版挪开后的 x | Values from that real run: original x, shift, x after the original game moved it away
int main(void) {
 unsigned char *base=calloc(1,0x2f05460); if(!base)return 2;
 g_nmod=1; strcpy(g_mod[0].name,"main"); g_mod[0].base=base;
 *(uint64_t *)(base+0x2f05458)=ptr(root); *(uint64_t *)(root+0x58)=ptr(children); *(uint64_t *)(root+0x60)=1;
 *(const char **)(inv+8)="PlayerInventoryExpansionLayout"; *x()=HOME; inv[0x50]=1; children[0]=ptr(inv);
 // 用 B 关：x 还是我们的值，当场写回 | Closed with B: x is still our value, restored at once
 mv_offset(MV_CUBE,ptr(inv),0,SHIFT,0); CHECK(*x()==HOME+SHIFT);
 mv_restore(ptr(root),MV_CUBE); CHECK(*x()==HOME && mv_count(MV_CUBE)==0);
 for(int k=0;k<4;k++) {
  // 用 ＋ 关：开储藏箱右移，原版接着把物品栏挪到单开位置 | Closed with +: shifted while the stash is open, then the original game moves the inventory to the stand-alone position
  mv_offset(MV_CUBE,ptr(inv),0,SHIFT,0); CHECK(*x()==HOME+SHIFT);
  *x()=ALONE;
  mv_restore(ptr(root),MV_CUBE); CHECK(*x()==ALONE);   // 原版的位置不动 | the original game's position is left alone
  mv_restore(ptr(root),MV_CUBE); CHECK(*x()==ALONE);
  // 单开的物品栏关掉：原版写回它记的 x（我们挪过的值），这时要写回原位 | The stand-alone inventory closes: the original game writes back the x it recorded (our shifted value); now the original must be restored
  *x()=HOME+SHIFT;
  mv_restore(ptr(root),MV_CUBE); CHECK(*x()==HOME && mv_count(MV_CUBE)==0);
 }
 // 等的期间原版重排出别的原位：下次开储藏箱按新原位挪 | While waiting the original game lays out a new original position: the next stash open shifts from it
 mv_offset(MV_CUBE,ptr(inv),0,SHIFT,0); *x()=ALONE; mv_restore(ptr(root),MV_CUBE);
 *x()=40; mv_offset(MV_CUBE,ptr(inv),0,SHIFT,0); CHECK(*x()==40+SHIFT);
 mv_restore(ptr(root),MV_CUBE); CHECK(*x()==40 && mv_count(MV_CUBE)==0);
 free(base); printf("t_mv_restore: %d checks, %d failures\n",checks,fails); return fails!=0;
}
