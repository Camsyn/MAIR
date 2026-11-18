mair graph of fn @u8_below {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read ptr %a, 200

}

mair graph of fn @u8_below of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_exact {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read ptr %a, 256

}

mair graph of fn @u8_exact of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_over {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: write ptr %a, 256

}

mair graph of fn @u8_over of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_symbolic {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read ptr %a, (256 umin (1 umax zext(%len to i65)))

}

mair graph of fn @u8_symbolic of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u16_elem4 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: write ptr %a, (4 * (65536 umin (1 umax zext(%len to i65)))), align 4

}

mair graph of fn @u16_elem4 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_field {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=8, access-size=4, trips=256, align 4

}

mair graph of fn @u8_field of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_until_zero {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read ptr %a, (256 umin (1 + zext(backedges(%loop) to i65)))

}

mair graph of fn @u8_until_zero of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_start_k {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @u8_start_k of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %a + %idx, 1

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_plus_k {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @u8_plus_k of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %a + %idx, 1

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @s8_index {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @s8_index of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %a + %idx, 1

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_step2 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=2, access-size=1, trips=(128 umin (1 umax zext(%len to i65)))

}

mair graph of fn @u8_step2 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_step3 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @u8_step3 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %a + %idx, 1

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_phase1_step4 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: write period start=(%a + 4), stride=16, access-size=4, trips=(64 umin (1 umax zext(%len to i65))), align 4

}

mair graph of fn @u8_phase1_step4 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_phase4_step4 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @u8_phase4_step4 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: write ptr %a + %p, 4, align 4

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_down {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read ptr %a, 256

}

mair graph of fn @u8_down of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_down_symbolic {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: write period start=(%a + 1020), stride=-4, access-size=4, trips=(256 umin (1 umax zext(%len to i65))), align 4

}

mair graph of fn @u8_down_symbolic of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_down_from_254 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @u8_down_from_254 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %a + %idx, 1

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_pitch {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=zext(%pitch to i64), access-size=1, trips=(256 umin (1 umax zext(%len to i65)))

}

mair graph of fn @u8_pitch of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_signed_pitch {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @u8_signed_pitch of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %a + %off, 1

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_wide_pitch {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @u8_wide_pitch of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %a + %off, 1

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

