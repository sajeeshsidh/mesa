// Copyright © 2023 Mel Henning
// SPDX-License-Identifier: MIT

use crate::cfg::{CFGBuilder, CFG};
use crate::ir::*;
use std::collections::HashMap;
use std::collections::HashSet;

fn clone_branch(op: &Op) -> Op {
    match op {
        Op::Bra(b) => Op::Bra(b.clone()),
        Op::Exit(e) => Op::Exit(e.clone()),
        _ => unreachable!(),
    }
}

fn jump_thread(func: &mut Function) -> bool {
    // Let's call a basic block "trivial" if its only instruction is an
    // unconditional branch. If a block is trivial, we can update all of its
    // predecessors to jump to its sucessor.
    //
    // A single reverse pass over the basic blocks is enough to update all of
    // the edges we're interested in. Roughly, if we assume that all loops in
    // the shader can terminate, then loop heads are never trivial and we
    // never replace a backward edge. Therefore, in each step we only need to
    // make sure that later control flow has been replaced in order to update
    // the current block as much as possible.
    //
    // We additionally try to update a branch-to-empty-block to point to the
    // block's successor, which along with block dce/reordering can sometimes
    // enable a later optimization that converts branches to fallthrough.
    let mut progress = false;

    // A branch to label can be replaced with Op
    let mut replacements: HashMap<Label, Op> = HashMap::new();

    // Invariant 1: At the end of each loop iteration,
    //              every trivial block with an index in [i, blocks.len())
    //              is represented in replacements.keys()
    // Invariant 2: replacements.values() never contains
    //              a branch to a trivial block
    for i in (0..func.blocks.len()).rev() {
        // Replace the branch if possible
        if let Some(instr) = func.blocks[i].instrs.last_mut() {
            if let Op::Bra(OpBra { target }) = instr.op {
                if let Some(replacement) = replacements.get(&target) {
                    instr.op = clone_branch(replacement);
                    progress = true;
                }
                // If the branch target was previously a trivial block then the
                // branch was previously a forward edge (see above) and by
                // invariants 1 and 2 we just updated the branch to target
                // a nontrivial block
            }
        }

        // Is this block trivial?
        let block_label = func.blocks[i].label;
        match &func.blocks[i].instrs[..] {
            [instr] => {
                if instr.is_branch() && instr.pred.is_true() {
                    // Upholds invariant 2 because we updated the branch above
                    replacements.insert(block_label, clone_branch(&instr.op));
                }
            }
            [] => {
                // Empty block - falls through
                // Our successor might be trivial, so we need to
                // apply the rewrite map to uphold invariant 2
                let target_label = func.blocks[i + 1].label;
                let replacement = replacements
                    .get(&target_label)
                    .map(clone_branch)
                    .unwrap_or_else(|| {
                        Op::Bra(OpBra {
                            target: target_label,
                        })
                    });
                replacements.insert(block_label, replacement);
            }
            _ => (),
        }
    }

    if progress {
        // We don't update the CFG above, so rewrite it if we made progress
        func.blocks = rewrite_cfg(std::mem::take(&mut func.blocks).into());
    }

    progress
}

fn rewrite_cfg(blocks: Vec<BasicBlock>) -> CFG<BasicBlock> {
    // CFGBuilder takes care of removing dead blocks for us
    // We use the basic block's label to identify it
    let mut builder = CFGBuilder::new();

    for i in 0..blocks.len() {
        let block = &blocks[i];
        // Note: fall-though must be first edge
        if block.falls_through() {
            let next_block = &blocks[i + 1];
            builder.add_edge(block.label, next_block.label);
        }
        if let Some(control_flow) = block.branch() {
            match &control_flow.op {
                Op::Bra(bra) => {
                    builder.add_edge(block.label, bra.target);
                }
                Op::Exit(_) => (),
                _ => unreachable!(),
            };
        }
    }

    for block in blocks {
        builder.add_node(block.label, block);
    }
    builder.as_cfg()
}

/// Replace jumps to the following block with fall-through
fn opt_fall_through(func: &mut Function) {
    for i in 0..func.blocks.len() - 1 {
        let remove_last_instr = match func.blocks[i].branch() {
            Some(b) => match b.op {
                Op::Bra(OpBra { target }) => target == func.blocks[i + 1].label,
                _ => false,
            },
            None => false,
        };

        if remove_last_instr {
            func.blocks[i].instrs.pop();
        }
    }
}

impl Function {
    pub fn opt_jump_thread(&mut self) {
        if jump_thread(self) {
            opt_fall_through(self);
        }
    }
}

impl Shader {
    /// A simple jump threading pass
    ///
    /// Note that this can introduce critical edges, so it cannot be run before RA
    pub fn opt_jump_thread(&mut self) {
        for f in &mut self.functions {
            f.opt_jump_thread();
        }
    }
}

fn merge_blocks(func: &mut Function) {
    let mut to_merge = HashSet::new();
    for i in 1..func.blocks.len() {
        let &[pred_i] = func.blocks.pred_indices(i) else {
            continue;
        };
        if func.blocks.succ_indices(pred_i).len() != 1 {
            continue;
        }

        // CFG construction should reorder these blocks to be adjacent
        assert!(pred_i == i - 1);
        // so there is no jump, thanks to opt_fall_through()
        assert!(func.blocks[pred_i].branch().is_none());

        to_merge.insert(func.blocks[i].label);

        // Handle phis
        if func.blocks[i].phi_dsts().is_some() {
            let mut phi_to_dst = HashMap::<u32, Dst>::new();
            if let Op::PhiDsts(phi_dsts) = func.blocks[i].instrs.remove(0).op {
                for (&p, &d) in phi_dsts.dsts.iter() {
                    phi_to_dst.insert(p, d);
                }
            } else {
                unreachable!();
            };

            let pred = &mut func.blocks[pred_i];
            if let Op::PhiSrcs(phi_srcs) = pred.instrs.pop().unwrap().op {
                pred.instrs.extend(phi_srcs.srcs.iter().map(|(p, s)| {
                    Instr::new_boxed(OpCopy {
                        src: *s,
                        dst: phi_to_dst[p],
                    })
                }));
            } else {
                unreachable!();
            }
        }
    }

    if to_merge.is_empty() {
        return;
    }

    let mut blocks: Vec<_> = std::mem::take(&mut func.blocks).into();
    blocks.dedup_by(|second, first| {
        if to_merge.contains(&second.label) {
            first.instrs.extend(std::mem::take(&mut second.instrs));
            true
        } else {
            false
        }
    });
    func.blocks = rewrite_cfg(blocks);
}

impl Shader {
    /// A pass to merge basic blocks
    ///
    /// Merges basic blocks in cases like this:
    /// block a {
    ///    phi_srcs
    /// } // succs: [b]
    /// block b { // preds: [a]
    ///    phi_dsts
    /// }
    /// This can happen if B previously had an unreachable predecessor C that
    /// we removed during CFG construction
    ///
    /// We don't want to leave these around because they hinder optimization
    /// and are a weird edge case for the rest of the backend to deal with.
    pub fn merge_blocks(&mut self) {
        for f in &mut self.functions {
            opt_fall_through(f);
            merge_blocks(f);
        }
    }
}
