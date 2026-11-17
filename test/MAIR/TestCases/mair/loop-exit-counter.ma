mair graph of fn @reuse_i64 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=8, access-size=8, trips=(1 + zext(%i to i65)), align 8

}

mair graph of fn @reuse_i64 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @u8_counter {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=8, access-size=8, trips=(1 + zext(backedges(%loop) to i65)), align 8

}

mair graph of fn @u8_counter of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @narrow_counters {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp10(pdom)], succs=[pp0(pdom), pp10(dom)]

pp8: preds=[pp11(dom), pp9(pdom)], succs=[pp11(pdom), pp9(dom)]

pp9: preds=[pp8(dom)], succs=[pp8(pdom)]

pp10: loop-preheader=%loop, preds=[pp1(dom), pp11(pdom)], succs=[pp1(pdom), pp11(dom)]

pp11: loop-exit=%loop, preds=[pp8(pdom), pp10(dom)], succs=[pp8(dom), pp10(pdom)]
  o: read period start=%key, stride=8, access-size=8, trips=(1 + zext(backedges(%loop) to i65)), align 8
  o: write period start=%d, stride=8, access-size=8, trips=zext(backedges(%loop) to i65), align 8

}

mair graph of fn @narrow_counters of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom), pp7(pdom iter %loop)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom), pp5(dom)]

pp5: preds=[pp6(pdom), pp4(dom)], succs=[pp6(dom)]

pp6: preds=[pp5(dom), pp7(pdom)], succs=[pp5(pdom), pp7(dom)]

pp7: preds=[pp6(dom), pp2(pdom iter %loop)], succs=[pp6(pdom)]

}

mair graph of fn @pointer_only {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=4, access-size=4, trips=(1 + zext(backedges(%loop) to i65)), align 4

}

mair graph of fn @pointer_only of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @strided_i64 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=16, access-size=8, trips=(1 + zext(backedges(%loop) to i65)), align 8

}

mair graph of fn @strided_i64 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @proven_i32 {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=8, access-size=8, trips=(1 + zext(%i to i33)), align 8

}

mair graph of fn @proven_i32 of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @no_counter_for_start {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp12(pdom)], succs=[pp0(pdom), pp12(dom)]

pp10: preds=[pp13(dom), pp11(pdom)], succs=[pp13(pdom), pp11(dom)]

pp11: preds=[pp10(dom)], succs=[pp10(pdom)]

pp12: loop-preheader=%outer, preds=[pp1(dom), pp13(pdom)], succs=[pp1(pdom), pp13(dom)]

pp13: loop-exit=%outer, preds=[pp10(pdom), pp12(dom)], succs=[pp10(dom), pp12(pdom)]

}

mair graph of fn @no_counter_for_start of loop %outer {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp14(pdom)], succs=[pp2(pdom), pp14(dom)]

pp8: preds=[pp15(dom), pp9(pdom)], succs=[pp15(pdom), pp9(dom)]

pp9: preds=[pp8(dom)], succs=[pp8(pdom)]

pp14: loop-preheader=%loop, preds=[pp3(dom), pp15(pdom)], succs=[pp3(pdom), pp15(dom)]

pp15: loop-exit=%loop, preds=[pp8(pdom), pp14(dom)], succs=[pp8(dom), pp14(pdom)]
  o: read period start=%s, stride=1, access-size=1, trips=(1 + zext(((18446744073709551615 * ptrtoint(%x to i64)) + ptrtoint(%pn to i64)) to i65))

}

mair graph of fn @no_counter_for_start of loop %loop {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp6(pdom)], succs=[pp4(pdom), pp6(dom)]

pp6: preds=[pp5(dom), pp7(pdom)], succs=[pp5(pdom), pp7(dom)]
  o: write ptr %pn + 1, 1

pp7: preds=[pp6(dom)], succs=[pp6(pdom)]

}

