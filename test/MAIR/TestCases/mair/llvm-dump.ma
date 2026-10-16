mair graph of fn @f {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp2(pdom)], succs=[pp0(pdom), pp2(dom)]
  o: read ptr %p, 4

pp2: preds=[pp1(dom), pp3(pdom)], succs=[pp1(pdom), pp3(dom)]
  o: write ptr %p, 4

pp3: preds=[pp2(dom)], succs=[pp2(pdom)]

}

mair graph of fn @branch_loop {
pp0: preds=[pp1(pdom)], succs=[pp1(dom)]

pp1: preds=[pp0(dom), pp6(pdom)], succs=[pp0(pdom), pp2(dom), pp6(dom)]

pp2: preds=[pp3(pdom), pp1(dom)], succs=[pp3(dom)]

pp3: preds=[pp2(dom), pp4(pdom)], succs=[pp2(pdom), pp4(dom)]
  o: read ptr %p, 4

pp4: preds=[pp3(dom), pp5(pdom)], succs=[pp3(pdom), pp5(dom)]
  o: write ptr %q, 4

pp5: preds=[pp4(dom), pp6(pdom)], succs=[pp4(pdom)]

pp6: preds=[pp7(pdom), pp1(dom)], succs=[pp7(dom), pp1(pdom), pp5(pdom)]

pp7: preds=[pp6(dom), pp8(pdom)], succs=[pp6(pdom), pp8(dom)]
  o: read ptr %p, 4

pp8: preds=[pp7(dom), pp9(pdom)], succs=[pp7(pdom), pp9(dom)]
  o: write ptr %q, 4

pp9: preds=[pp8(dom), pp10(pdom)], succs=[pp8(pdom), pp10(dom)]

pp10: preds=[pp11(pdom), pp9(dom)], succs=[pp11(dom), pp9(pdom)]

pp11: preds=[pp10(dom)], succs=[pp10(pdom)]

}

