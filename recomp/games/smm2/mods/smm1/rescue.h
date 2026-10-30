// SMM1's rescue scene, NetEndingDemoScene (rescue.c): Mario walks in from the left to Peach (or Toad on Easy)
// standing on the style's ground model, drawn from the SMM1 dump's models (research/specs/smm1-ending.md §2).
#pragma once
#include "../hundred.h"
#include "lyt.h"

typedef struct Rescue Rescue;
// style: 0 SMB1, 1 SMB3, 2 SMW, 3 NSMBU (the cleared course's); peach: Normal and up (else Toad); look: the players at
// the goal (power-ups, Goomba's Shoe / Yoshi, co-op's Luigi) and the course's theme, NULL for small Mario alone
Rescue *rescue_new(int style, bool peach, const struct hundred_look *look);
void rescue_free(Rescue *r);
int rescue_length(const Rescue *r);    // frames until the scene asks for the credits (818; 998 NSMBU with Peach)
int rescue_text_in(const Rescue *r);   // the frame the balloon text comes in (Mario has stopped)
int rescue_text_out(const Rescue *r);  // the frame its Out starts
// where the balloon points (layout units, 1280 x 720, y up): above DemoPeach's spot with Mario stopped [obs: Cemu]
void rescue_balloon_anchor(const Rescue *r, float xy[2]);
// the scene at frame t (0 = its start) in layout units (1280 x 720), with the colour behind it
void rescue_draw(Rescue *r, int t, LQuads *out, uint8_t bg[4]);
