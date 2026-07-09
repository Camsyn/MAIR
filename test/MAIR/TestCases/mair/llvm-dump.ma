mair graph of fn @f {
bs0: preds=[ma1(pdom)], succs=[ma1(dom)]

ma1: preds=[bs0(dom), ma2(pdom)], succs=[bs0(pdom), ma2(dom)]
  read ptr %p, 4

ma2: preds=[ma1(dom), be3(pdom)], succs=[ma1(pdom), be3(dom)]
  write ptr %p, 4

be3: preds=[ma2(dom)], succs=[ma2(pdom)]

}

mair graph of fn @branch_loop {
bs0: preds=[be1(pdom)], succs=[be1(dom), bs2(dom), bs6(dom)]

be1: preds=[bs0(dom), bs6(pdom)], succs=[bs0(pdom)]

bs2: preds=[ma3(pdom), bs0(dom)], succs=[ma3(dom)]

ma3: preds=[bs2(dom), ma4(pdom)], succs=[bs2(pdom), ma4(dom)]
  read ptr %p, 4

ma4: preds=[ma3(dom), be5(pdom)], succs=[ma3(pdom), be5(dom)]
  write ptr %q, 4

be5: preds=[ma4(dom), bs6(pdom)], succs=[ma4(pdom)]

bs6: preds=[ma7(pdom), bs0(dom)], succs=[ma7(dom), be1(pdom), be5(pdom), bs10(dom)]

ma7: preds=[bs6(dom), ma8(pdom)], succs=[bs6(pdom), ma8(dom)]
  read ptr %p, 4

ma8: preds=[ma7(dom), be9(pdom)], succs=[ma7(pdom), be9(dom)]
  write ptr %q, 4

be9: preds=[ma8(dom), bs10(pdom)], succs=[ma8(pdom)]

bs10: preds=[be11(pdom), bs6(dom)], succs=[be11(dom), be9(pdom)]

be11: preds=[bs10(dom)], succs=[bs10(pdom)]

}

