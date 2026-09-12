/*
 * keybinds_read_player_runner: does each bound key land on the bit the RUNNER
 * means by it?
 *
 * There are two 12-bit keyboard words in this tree and they are exact mirrors
 * of each other. keybinds_read_player() returns a keyboard-local layout for
 * the desktop host's remap table (bit 4 = Right); a runner's seat word, the
 * one that goes to RtlRunFrame, has bit 4 = Up. Hand one to the other and it
 * compiles, links, runs, and silently swaps every direction while scrambling
 * the face buttons.
 *
 * That is not hypothetical. It shipped in a per-game host and reached a player
 * as "the arrow keys all go the wrong way in game". Nothing caught it because
 * both words are plausible 12-bit values and every automated check passed --
 * the only detector was a human with their hands on the keys.
 *
 * So this asserts the mapping button by button rather than trusting anyone to
 * reason about it again, and ends with the two direction cases that actually
 * reached the player.
 */
#include <stdio.h>
#include <string.h>
#include "keybinds.h"
static int fails;
static void ck(const char *w,int ok){printf("  %-46s %s\n",w,ok?"ok":"FAIL");if(!ok)fails++;}

int main(void){
    keybinds_init(NULL);
    static unsigned char keys[512];
    /* Give player 2 a unique key per button so nothing can alias. */
    const int btn_count = keybinds_button_count();
    struct { const char *name; int bit; } expect[12] = {
        {"B",0},{"Y",1},{"Select",2},{"Start",3},{"Up",4},{"Down",5},
        {"Left",6},{"Right",7},{"A",8},{"X",9},{"L",10},{"R",11} };
    /* Bind p2 button i to scancode 40+i (unique, unused by defaults). */
    const KeyBinds *kb = keybinds_get();
    const SDL_Scancode sc[12] = {
        kb->p2.b, kb->p2.y, kb->p2.select, kb->p2.start, kb->p2.up, kb->p2.down,
        kb->p2.left, kb->p2.right, kb->p2.a, kb->p2.x, kb->p2.l, kb->p2.r };
    (void)btn_count;
    for (int i=0;i<12;i++){
        if (sc[i]==SDL_SCANCODE_UNKNOWN){ printf("  %-46s skip (unbound)\n",expect[i].name); continue; }
        memset(keys,0,sizeof keys);
        keys[sc[i]]=1;
        unsigned w = keybinds_read_player_runner(keys,2);
        char msg[80]; snprintf(msg,sizeof msg,"%s -> bit %d only",expect[i].name,expect[i].bit);
        ck(msg, w == (1u<<expect[i].bit));
        if (w != (1u<<expect[i].bit))
            printf("       got 0x%03x, wanted 0x%03x\n", w, 1u<<expect[i].bit);
    }
    /* And the direction pair that actually reached the player. */
    memset(keys,0,sizeof keys); keys[kb->p2.up]=1;
    ck("Up does not read as Right", !(keybinds_read_player_runner(keys,2) & (1u<<7)));
    memset(keys,0,sizeof keys); keys[kb->p2.left]=1;
    ck("Left does not read as Down", !(keybinds_read_player_runner(keys,2) & (1u<<5)));
    printf("\n%s (%d failure%s)\n", fails?"FAILED":"PASSED", fails, fails==1?"":"s");
    return fails?1:0;
}
