mair graph of fn @byte_index {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp2(pdom)], succs=[pp0(pdom), pp2(dom)]

pp2: preds=[pp1(dom)], succs=[pp1(pdom)]

}

mair graph of fn @byte_index_plus_one {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp2(pdom)], succs=[pp0(pdom), pp2(dom)]
  o: read ptr @T + %q, 1

pp2: preds=[pp1(dom)], succs=[pp1(pdom)]

}

mair graph of fn @mutable_table {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp2(pdom)], succs=[pp0(pdom), pp2(dom)]
  o: read ptr @M + %i, 1

pp2: preds=[pp1(dom)], succs=[pp1(pdom)]

}

mair graph of fn @replaceable_tables {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp2(pdom)], succs=[pp0(pdom), pp2(dom)]
  o: read ptr @X + %i, 1

pp2: preds=[pp1(dom), pp3(pdom)], succs=[pp1(pdom), pp3(dom)]
  o: read ptr @Weak + %i, 1

pp3: preds=[pp2(dom)], succs=[pp2(pdom)]

}

mair graph of fn @masked_word_index {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp2(pdom)], succs=[pp0(pdom), pp2(dom)]

pp2: preds=[pp1(dom)], succs=[pp1(pdom)]

}

mair graph of fn @guarded_index {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom)], succs=[pp0(pdom), pp2(dom), pp5(dom)]

pp2: preds=[pp3(pdom), pp1(dom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

pp5: preds=[pp6(pdom), pp1(dom)], succs=[pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

}

mair graph of fn @lookup_loop {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp10(pdom)], succs=[pp0(pdom), pp2(dom), pp10(dom)]

pp2: preds=[pp3(pdom), pp1(dom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp12(pdom)], succs=[pp2(pdom), pp12(dom)]

pp8: preds=[pp13(dom), pp9(pdom)], succs=[pp13(pdom), pp9(dom)]

pp9: preds=[pp8(dom), pp10(pdom)], succs=[pp8(pdom)]

pp10: preds=[pp11(pdom), pp1(dom)], succs=[pp11(dom), pp1(pdom), pp9(pdom)]

pp11: preds=[pp10(dom)], succs=[pp10(pdom)]

pp12: loop-preheader=%loop, preds=[pp3(dom), pp13(pdom)], succs=[pp3(pdom), pp13(dom)]

pp13: loop-exit=%loop, preds=[pp8(pdom), pp12(dom)], succs=[pp8(dom), pp12(pdom)]
  o: read ptr %s, (1 + zext((18446744073709551615 + %n) to i65))

}

mair graph of fn @lookup_loop of loop %loop {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp6(pdom)], succs=[pp4(pdom), pp6(dom)]

pp6: preds=[pp5(dom), pp7(pdom)], succs=[pp5(pdom), pp7(dom)]

pp7: preds=[pp6(dom)], succs=[pp6(pdom)]

}

mair graph of fn @bounded_range {
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

}

mair graph of fn @bounded_range of loop %loop {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp6(pdom)], succs=[pp4(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

}

mair graph of fn @unbounded_range {
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
  o: read volatile ptr @T, (1 + zext((18446744073709551615 + %n) to i65))

}

mair graph of fn @unbounded_range of loop %loop {
pp4: preds=[pp5(pdom)], succs=[pp5(dom)]

pp5: preds=[pp4(dom), pp6(pdom)], succs=[pp4(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

}

mair graph of fn @periodic {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp7(pdom)], succs=[pp0(pdom), pp7(dom)]

pp5: preds=[pp8(dom), pp6(pdom)], succs=[pp8(pdom), pp6(dom)]

pp6: preds=[pp5(dom)], succs=[pp5(pdom)]

pp7: loop-preheader=%loop, preds=[pp1(dom), pp8(pdom)], succs=[pp1(pdom), pp8(dom)]

pp8: loop-exit=%loop, preds=[pp5(pdom), pp7(dom)], succs=[pp5(dom), pp7(pdom)]

}

mair graph of fn @periodic of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom)], succs=[pp3(pdom)]

}

mair graph of fn @periodic_end {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp8(pdom)], succs=[pp0(pdom), pp8(dom)]

pp6: preds=[pp9(dom), pp7(pdom)], succs=[pp9(pdom), pp7(dom)]

pp7: preds=[pp6(dom)], succs=[pp6(pdom)]

pp8: loop-preheader=%loop, preds=[pp1(dom), pp9(pdom)], succs=[pp1(pdom), pp9(dom)]

pp9: loop-exit=%loop, preds=[pp6(pdom), pp8(dom)], succs=[pp6(dom), pp8(pdom)]
  o: read volatile period start=(@T + 1), stride=3, access-size=1, trips=86

}

mair graph of fn @periodic_end of loop %loop {
pp2: preds=[pp3(pdom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]

pp4: preds=[pp3(dom), pp5(pdom)], succs=[pp3(pdom), pp5(dom)]

pp5: preds=[pp4(dom)], succs=[pp4(pdom)]

}

