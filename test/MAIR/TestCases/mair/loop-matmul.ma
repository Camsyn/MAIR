mair graph of fn @mm_const {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp17(pdom)], succs=[pp0(pdom), pp17(dom)]

pp15: preds=[pp18(dom), pp16(pdom)], succs=[pp18(pdom), pp16(dom)]

pp16: preds=[pp15(dom)], succs=[pp15(pdom)]

pp17: loop-preheader=%i.header, preds=[pp1(dom), pp18(pdom)], succs=[pp1(pdom), pp18(dom)]
  o: read ptr %B, 16384, align 4

pp18: loop-exit=%i.header, preds=[pp15(pdom), pp17(dom)], succs=[pp15(dom), pp17(pdom)]
  o: read ptr %A, 16384, align 4
  o: write ptr %C, 16384, align 4

}

mair graph of fn @mm_const of loop %i.header {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp19(pdom)], succs=[pp2(pdom), pp19(dom)]

pp13: preds=[pp20(dom), pp14(pdom)], succs=[pp20(pdom), pp14(dom)]

pp14: preds=[pp13(dom)], succs=[pp13(pdom)]

pp19: loop-preheader=%j.header, preds=[pp3(dom), pp20(pdom)], succs=[pp3(pdom), pp20(dom)]

pp20: loop-exit=%j.header, preds=[pp13(pdom), pp19(dom)], succs=[pp13(dom), pp19(pdom)]

}

mair graph of fn @mm_const of loop %j.header {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp21(pdom)], succs=[pp4(pdom), pp21(dom)]

pp10: preds=[pp22(dom), pp11(pdom)], succs=[pp22(pdom), pp11(dom)]

pp11: preds=[pp10(dom), pp12(pdom)], succs=[pp10(pdom), pp12(dom)]

pp12: preds=[pp11(dom)], succs=[pp11(pdom)]

pp21: loop-preheader=%k.body, preds=[pp5(dom), pp22(pdom)], succs=[pp5(pdom), pp22(dom)]

pp22: loop-exit=%k.body, preds=[pp10(pdom), pp21(dom)], succs=[pp10(dom), pp21(pdom)]

}

mair graph of fn @mm_const of loop %k.body {
pp6: preds=[pp8(pdom)], succs=[pp8(dom)]

pp8: preds=[pp9(pdom), pp6(dom)], succs=[pp9(dom), pp6(pdom)]

pp9: preds=[pp8(dom)], succs=[pp8(pdom)]

}

mair graph of fn @mm_const_unrolled {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp23(pdom)], succs=[pp0(pdom), pp23(dom)]

pp21: preds=[pp24(dom), pp22(pdom)], succs=[pp24(pdom), pp22(dom)]

pp22: preds=[pp21(dom)], succs=[pp21(pdom)]

pp23: loop-preheader=%i.header, preds=[pp1(dom), pp24(pdom)], succs=[pp1(pdom), pp24(dom)]
  o: read ptr %B, 16384, align 4

pp24: loop-exit=%i.header, preds=[pp21(pdom), pp23(dom)], succs=[pp21(dom), pp23(pdom)]
  o: read ptr %A, 16384, align 4
  o: write ptr %C, 16384, align 4

}

mair graph of fn @mm_const_unrolled of loop %i.header {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp25(pdom)], succs=[pp2(pdom), pp25(dom)]

pp19: preds=[pp26(dom), pp20(pdom)], succs=[pp26(pdom), pp20(dom)]

pp20: preds=[pp19(dom)], succs=[pp19(pdom)]

pp25: loop-preheader=%j.header, preds=[pp3(dom), pp26(pdom)], succs=[pp3(pdom), pp26(dom)]

pp26: loop-exit=%j.header, preds=[pp19(pdom), pp25(dom)], succs=[pp19(dom), pp25(pdom)]

}

mair graph of fn @mm_const_unrolled of loop %j.header {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp27(pdom)], succs=[pp4(pdom), pp27(dom)]

pp16: preds=[pp28(dom), pp17(pdom)], succs=[pp28(pdom), pp17(dom)]

pp17: preds=[pp16(dom), pp18(pdom)], succs=[pp16(pdom), pp18(dom)]

pp18: preds=[pp17(dom)], succs=[pp17(pdom)]

pp27: loop-preheader=%k.body, preds=[pp5(dom), pp28(pdom)], succs=[pp5(pdom), pp28(dom)]

pp28: loop-exit=%k.body, preds=[pp16(pdom), pp27(dom)], succs=[pp16(dom), pp27(pdom)]

}

mair graph of fn @mm_const_unrolled of loop %k.body {
pp6: preds=[pp14(pdom)], succs=[pp14(dom)]

pp14: preds=[pp15(pdom), pp6(dom)], succs=[pp15(dom), pp6(pdom)]

pp15: preds=[pp14(dom)], succs=[pp14(pdom)]

}

mair graph of fn @mm {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp19(pdom)], succs=[pp0(pdom), pp2(dom), pp19(dom)]

pp2: preds=[pp3(pdom), pp1(dom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp21(pdom)], succs=[pp2(pdom), pp21(dom)]

pp17: preds=[pp22(dom), pp18(pdom)], succs=[pp22(pdom), pp18(dom)]

pp18: preds=[pp17(dom), pp19(pdom)], succs=[pp17(pdom)]

pp19: preds=[pp20(pdom), pp1(dom)], succs=[pp20(dom), pp1(pdom), pp18(pdom)]

pp20: preds=[pp19(dom)], succs=[pp19(pdom)]

pp21: loop-preheader=%i.header, preds=[pp3(dom), pp22(pdom)], succs=[pp3(pdom), pp22(dom)]
  o: read ptr %B, ((4 * (1 + zext((18446744073709551615 + %n) to i131))) * (1 + zext((18446744073709551615 + %n) to i131))), align 4

pp22: loop-exit=%i.header, preds=[pp17(pdom), pp21(dom)], succs=[pp17(dom), pp21(pdom)]
  o: read ptr %A, ((1 + zext((18446744073709551615 + %n) to i131)) * (4 + (4 * zext((18446744073709551615 + %n) to i131)))), align 4
  o: write ptr %C, ((1 + zext((18446744073709551615 + %n) to i131)) * (4 + (4 * zext((18446744073709551615 + %n) to i131)))), align 4

}

mair graph of fn @mm of loop %i.header {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp23(pdom)], succs=[pp4(pdom), pp23(dom)]

pp15: preds=[pp24(dom), pp16(pdom)], succs=[pp24(pdom), pp16(dom)]

pp16: preds=[pp15(dom)], succs=[pp15(pdom)]

pp23: loop-preheader=%j.header, preds=[pp5(dom), pp24(pdom)], succs=[pp5(pdom), pp24(dom)]

pp24: loop-exit=%j.header, preds=[pp15(pdom), pp23(dom)], succs=[pp15(dom), pp23(pdom)]

}

mair graph of fn @mm of loop %j.header {
pp6: preds=[pp7(pdom)], succs=[pp7(dom)]

pp7: preds=[pp6(dom), pp25(pdom)], succs=[pp6(pdom), pp25(dom)]

pp12: preds=[pp26(dom), pp13(pdom)], succs=[pp26(pdom), pp13(dom)]

pp13: preds=[pp12(dom), pp14(pdom)], succs=[pp12(pdom), pp14(dom)]

pp14: preds=[pp13(dom)], succs=[pp13(pdom)]

pp25: loop-preheader=%k.body, preds=[pp7(dom), pp26(pdom)], succs=[pp7(pdom), pp26(dom)]

pp26: loop-exit=%k.body, preds=[pp12(pdom), pp25(dom)], succs=[pp12(dom), pp25(pdom)]

}

mair graph of fn @mm of loop %k.body {
pp8: preds=[pp10(pdom)], succs=[pp10(dom)]

pp10: preds=[pp11(pdom), pp8(dom)], succs=[pp11(dom), pp8(pdom)]

pp11: preds=[pp10(dom)], succs=[pp10(pdom)]

}

