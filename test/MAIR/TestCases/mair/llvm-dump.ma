mair graph of fn @f {
bs0: preds=[ma1], succs=[ma1]

ma1: preds=[bs0, ma2], succs=[bs0, ma2]
  read ptr %p, 4

ma2: preds=[ma1, be3], succs=[ma1, be3]
  write ptr %p, 4

be3: preds=[ma2], succs=[ma2]

}

mair graph of fn @branch_loop {
bs0: preds=[be1], succs=[be1, bs2, bs6]

be1: preds=[bs0], succs=[bs0, bs6]

bs2: preds=[ma3, bs0], succs=[ma3]

ma3: preds=[bs2, ma4], succs=[bs2, ma4]
  read ptr %p, 4

ma4: preds=[ma3, be5], succs=[ma3, be5]
  write ptr %q, 4

be5: preds=[ma4], succs=[ma4, bs6]

bs6: preds=[ma7, be1, be5, bs0], succs=[ma7, bs10]

ma7: preds=[bs6, ma8], succs=[bs6, ma8]
  read ptr %p, 4

ma8: preds=[ma7, be9], succs=[ma7, be9]
  write ptr %q, 4

be9: preds=[ma8], succs=[ma8, bs10]

bs10: preds=[be11, be9, bs6], succs=[be11]

be11: preds=[bs10], succs=[bs10]

}

