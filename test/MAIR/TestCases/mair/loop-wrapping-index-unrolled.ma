mair graph of fn @unrolled_from_zero {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp15(pdom)], succs=[pp0(pdom), pp2(dom), pp15(dom)]

pp2: preds=[pp3(pdom), pp1(dom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp15(pdom)], succs=[pp2(pdom), pp4(dom), pp8(dom)]

pp4: preds=[pp5(pdom), pp3(dom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp23(pdom)], succs=[pp4(pdom), pp23(dom)]

pp6: preds=[pp24(dom), pp7(pdom)], succs=[pp24(pdom), pp7(dom)]

pp7: preds=[pp6(dom), pp15(pdom)], succs=[pp6(pdom)]

pp8: preds=[pp9(pdom), pp3(dom)], succs=[pp9(dom)]

pp9: preds=[pp8(dom), pp25(pdom)], succs=[pp8(pdom), pp25(dom)]

pp13: preds=[pp26(dom), pp14(pdom)], succs=[pp26(pdom), pp14(dom)]

pp14: preds=[pp13(dom), pp15(pdom)], succs=[pp13(pdom)]

pp15: preds=[pp16(pdom), pp1(dom)], succs=[pp16(dom), pp1(pdom), pp3(pdom), pp7(pdom), pp14(pdom)]

pp16: preds=[pp15(dom)], succs=[pp15(pdom)]

pp23: loop-preheader=%unrolled, preds=[pp5(dom), pp24(pdom)], succs=[pp5(pdom), pp24(dom)]

pp24: loop-exit=%unrolled, preds=[pp6(pdom), pp23(dom)], succs=[pp6(dom), pp23(pdom)]
  o: write period start=%a, stride=16, access-size=16, trips=(64 umin (1 + (zext((4294967292 + (4 * zext(trunc((%n udiv 4) to i29) to i32))) to i33) udiv 4))), align 4

pp25: loop-preheader=%epilogue, preds=[pp9(dom), pp26(pdom)], succs=[pp9(pdom), pp26(dom)]

pp26: loop-exit=%epilogue, preds=[pp13(pdom), pp25(dom)], succs=[pp13(dom), pp25(pdom)]

}

mair graph of fn @unrolled_from_zero of loop %unrolled {
pp17: preds=[pp21(pdom)], succs=[pp21(dom)]

pp21: preds=[pp22(pdom), pp17(dom)], succs=[pp22(dom), pp17(pdom)]

pp22: preds=[pp21(dom)], succs=[pp21(pdom)]

}

mair graph of fn @unrolled_from_zero of loop %epilogue {
pp10: preds=[pp11(pdom)], succs=[pp11(dom)]

pp11: preds=[pp10(dom), pp12(pdom)], succs=[pp10(pdom), pp12(dom)]
  o: write ptr %a + %pe, 4, align 4

pp12: preds=[pp11(dom)], succs=[pp11(pdom)]

}

mair graph of fn @unrolled_from_k {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp10(pdom)], succs=[pp0(pdom), pp10(dom)]

pp8: preds=[pp11(dom), pp9(pdom)], succs=[pp11(pdom), pp9(dom)]

pp9: preds=[pp8(dom)], succs=[pp8(pdom)]

pp10: loop-preheader=%loop, preds=[pp1(dom), pp11(pdom)], succs=[pp1(pdom), pp11(dom)]

pp11: loop-exit=%loop, preds=[pp8(pdom), pp10(dom)], succs=[pp8(dom), pp10(pdom)]

}

mair graph of fn @unrolled_from_k of loop %loop {
pp2: preds=[pp6(pdom)], succs=[pp6(dom)]

pp6: preds=[pp7(pdom), pp2(dom)], succs=[pp7(dom), pp2(pdom)]
  o: write ptr %a + %p0, 4, align 4
  o: write ptr %a + %p1, 4, align 4
  o: write ptr %a + %p2, 4, align 4
  o: write ptr %a + %p3, 4, align 4

pp7: preds=[pp6(dom)], succs=[pp6(pdom)]

}

