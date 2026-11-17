mair graph of fn @until_neg {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp8(pdom)], succs=[pp0(pdom), pp8(dom)]

pp5: preds=[pp9(dom), pp6(pdom)], succs=[pp9(pdom), pp6(dom)]

pp6: preds=[pp5(dom), pp7(pdom)], succs=[pp5(pdom), pp7(dom)]
  o: write ptr %out, 8, align 8

pp7: preds=[pp6(dom)], succs=[pp6(pdom)]

pp8: loop-preheader=%loop, preds=[pp1(dom), pp9(pdom)], succs=[pp1(pdom), pp9(dom)]

pp9: loop-exit=%loop, preds=[pp5(pdom), pp8(dom)], succs=[pp5(dom), pp8(pdom)]
  o: read ptr %a, (8 + (8 * zext(%i to i67))), align 8

}

mair graph of fn @until_neg of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @store_after_test {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp10(pdom)], succs=[pp0(pdom), pp10(dom)]

pp8: preds=[pp11(dom), pp9(pdom)], succs=[pp11(pdom), pp9(dom)]

pp9: preds=[pp8(dom)], succs=[pp8(pdom)]

pp10: loop-preheader=%header, preds=[pp1(dom), pp11(pdom)], succs=[pp1(pdom), pp11(dom)]

pp11: loop-exit=%header, preds=[pp8(pdom), pp10(dom)], succs=[pp8(dom), pp10(pdom)]
  o: read ptr %a, (8 + (8 * zext(%i.lcssa to i67))), align 8
  o: write ptr %b, (8 * zext(%i.lcssa to i66)), align 8

}

mair graph of fn @store_after_test of loop %header {
pp2: preds=[pp3(pdom)], succs=[pp3(dom), pp7(pdom iter %header)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom), pp5(dom)]

pp5: preds=[pp6(pdom), pp4(dom)], succs=[pp6(dom)]

pp6: preds=[pp5(dom), pp7(pdom)], succs=[pp5(pdom), pp7(dom)]

pp7: preds=[pp6(dom), pp2(pdom iter %header)], succs=[pp6(pdom)]

}

mair graph of fn @pointer_scan {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%s, stride=8, access-size=8, trips=(1 + (zext(((18446744073709551615 * ptrtoint(%s to i64)) + ptrtoint(%p to i64)) to i65) udiv 8)), align 8

}

mair graph of fn @pointer_scan of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @copy_until {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp8(pdom)], succs=[pp0(pdom), pp8(dom)]

pp6: preds=[pp9(dom), pp7(pdom)], succs=[pp9(pdom), pp7(dom)]

pp7: preds=[pp6(dom)], succs=[pp6(pdom)]

pp8: loop-preheader=%loop, preds=[pp1(dom), pp9(pdom)], succs=[pp1(pdom), pp9(dom)]

pp9: loop-exit=%loop, preds=[pp6(pdom), pp8(dom)], succs=[pp6(dom), pp8(pdom)]
  o: read period start=%s, stride=8, access-size=8, trips=(1 + (zext(((18446744073709551615 * ptrtoint(%d to i64)) + ptrtoint(%dp to i64)) to i65) udiv 8)), align 8
  o: write period start=%d, stride=8, access-size=8, trips=(1 + (zext(((18446744073709551615 * ptrtoint(%d to i64)) + ptrtoint(%dp to i64)) to i65) udiv 8)), align 8

}

mair graph of fn @copy_until of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom), pp5(pdom)], succs=[pp3(pdom), pp5(dom)]

pp5: preds=[pp4(dom)], succs=[pp4(pdom)]

}

mair graph of fn @int32_until {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read ptr %a, (4 + (4 * zext(%i to i34))), align 4

}

mair graph of fn @int32_until of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @unsigned_until {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read ptr %a, (8 + (8 * zext(%i to i68))), align 8

}

mair graph of fn @unsigned_until of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @pointer_range {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp9(pdom)], succs=[pp0(pdom), pp2(dom), pp9(dom)]

pp2: preds=[pp3(pdom), pp1(dom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp11(pdom)], succs=[pp2(pdom), pp11(dom)]

pp7: preds=[pp12(dom), pp8(pdom)], succs=[pp12(pdom), pp8(dom)]

pp8: preds=[pp7(dom), pp9(pdom)], succs=[pp7(pdom)]

pp9: preds=[pp10(pdom), pp1(dom)], succs=[pp10(dom), pp1(pdom), pp8(pdom)]

pp10: preds=[pp9(dom)], succs=[pp9(pdom)]

pp11: loop-preheader=%loop, preds=[pp3(dom), pp12(pdom)], succs=[pp3(pdom), pp12(dom)]

pp12: loop-exit=%loop, preds=[pp7(pdom), pp11(dom)], succs=[pp7(dom), pp11(pdom)]
  o: read period start=%b, stride=8, access-size=8, trips=(1 + (zext(((18446744073709551608 + (18446744073709551615 * ptrtoint(%b to i64))) + ptrtoint(%e to i64)) to i65) udiv 8)), align 8

}

mair graph of fn @pointer_range of loop %loop {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp6(pdom)], succs=[pp4(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

}

mair graph of fn @conditional_store {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp12(pdom)], succs=[pp0(pdom), pp12(dom)]

pp10: preds=[pp13(dom), pp11(pdom)], succs=[pp13(pdom), pp11(dom)]

pp11: preds=[pp10(dom)], succs=[pp10(pdom)]

pp12: loop-preheader=%loop, preds=[pp1(dom), pp13(pdom)], succs=[pp1(pdom), pp13(dom)]

pp13: loop-exit=%loop, preds=[pp10(pdom), pp12(dom)], succs=[pp10(dom), pp12(pdom)]
  o: read ptr %a, (8 + (8 * zext(%i to i67))), align 8

}

mair graph of fn @conditional_store of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom), pp8(pdom)], succs=[pp3(pdom), pp5(dom), pp8(dom)]

pp5: preds=[pp6(pdom), pp4(dom)], succs=[pp6(dom)]

pp6: preds=[pp5(dom), pp7(pdom)], succs=[pp5(pdom), pp7(dom)]
  o: write ptr %b + %pb, 8, align 8

pp7: preds=[pp6(dom), pp8(pdom)], succs=[pp6(pdom)]

pp8: preds=[pp9(pdom), pp4(dom)], succs=[pp9(dom), pp4(pdom), pp7(pdom)]

pp9: preds=[pp8(dom)], succs=[pp8(pdom)]

}

mair graph of fn @find {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp13(pdom)], succs=[pp0(pdom), pp2(dom), pp13(dom)]

pp2: preds=[pp3(pdom), pp1(dom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp15(pdom)], succs=[pp2(pdom), pp15(dom)]

pp9: preds=[pp16(dom), pp10(pdom)], succs=[pp16(pdom), pp10(dom)]

pp10: preds=[pp9(dom), pp13(pdom)], succs=[pp9(pdom)]

pp11: preds=[pp17(dom), pp12(pdom)], succs=[pp17(pdom), pp12(dom)]

pp12: preds=[pp11(dom), pp13(pdom)], succs=[pp11(pdom)]

pp13: preds=[pp14(pdom), pp1(dom)], succs=[pp14(dom), pp1(pdom), pp15(pdom), pp10(pdom), pp12(pdom)]

pp14: preds=[pp13(dom)], succs=[pp13(pdom)]

pp15: loop-preheader=%loop, preds=[pp3(dom), pp13(pdom)], succs=[pp3(pdom), pp16(dom), pp17(dom)]

pp16: loop-exit=%loop, preds=[pp9(pdom), pp15(dom)], succs=[pp9(dom)]

pp17: loop-exit=%loop, preds=[pp11(pdom), pp15(dom)], succs=[pp11(dom)]

}

mair graph of fn @find of loop %loop {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp6(pdom)], succs=[pp4(pdom), pp6(dom)]
  o: read ptr %a + %pa, 8, align 8

pp6: preds=[pp5(dom)], succs=[pp5(pdom), pp7(dom)]

pp7: preds=[pp8(pdom), pp6(dom)], succs=[pp8(dom)]

pp8: preds=[pp7(dom)], succs=[pp7(pdom)]

}

mair graph of fn @huge_stride {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]
  o: read period start=%a, stride=8192, access-size=1, trips=(1 + zext(%i to i65))

}

mair graph of fn @huge_stride of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

