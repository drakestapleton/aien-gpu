/-
  LeanVerify/AienGpuGb20b.lean
  Machine-checked safety and arithmetic invariants for the GB20B (Spark 0x1b/0xb)
  aien-gpu boot and Stage-C doorbell model.

  Model-to-system link:
    - Repository: aien-gpu, branch fix/gb20b-sec2-reply. Written 2026-11-18 by the
      antigravity-aien-gpu lane against head 39a2f15. This header was edited 2026-11-19; the
      definitions and proofs below are unchanged.
    - The definitions were written by hand from:
        * om/boot.om: ememdw, the reply size in brsp, the checks in brspchk, the order of the steps
        * om/rmarg.om op 4 (GB20B work-submit token, gb202/dev_vm.h:27-32)
        * om/rmchk.om (runlist bound 127)
        * tools/gb20b_gsp_boot.c (COT version 2 payload of 860 bytes, CrashCat fields at 224/232)
    - Nothing ties these definitions to the Omega units or the C mechanically: a change in the code
      does not fail these proofs. They prove properties of this model only. The code is checked by
      the tests in tests/host; on the chip everything is NOT_RUN.
    - Check: `lean docs/proofs/AienGpuGb20b.lean` with Lean 4.35.0 (no imports); docs/proofs/README.md.
-/

namespace AienGpuGb20b

/-! ## 1. SEC2 MSGQ 32-bit wraparound size formula (om/boot.om:251-256, 226) -/

def U32_MOD : Nat := 4294967296  -- 2^32 (Wolfram verified)

/-- Exact model of om/boot.om:251: `sz = and sub add add obs 4294967296 4 get st 11 4294967295` -/
def msgqSz (head tail : Nat) : Nat :=
  (tail + U32_MOD + 4 - head) % U32_MOD

/-- Exact model of gb20b_sim.c:82: `mtail = (head + bytes - 4) mod 2^32` -/
def simTail (head bytes : Nat) : Nat :=
  (head + bytes + U32_MOD - 4) % U32_MOD

/-- Unsigned 64-bit subtraction in om/boot.om:251 never underflows for any 32-bit head, tail. -/
theorem msgqSz_no_underflow (head tail : Nat) (hh : head < U32_MOD) :
    head ≤ tail + U32_MOD + 4 := by
  unfold U32_MOD at *; omega

/-- Matches 32-bit modular `(tail - head + 4) mod 2^32` for all 32-bit head, tail. -/
theorem msgqSz_eq_mod_sub_add4 (head tail : Nat) (hh : head < U32_MOD) :
    msgqSz head tail = ((tail + (U32_MOD - head)) + 4) % U32_MOD := by
  unfold msgqSz U32_MOD at *
  have h : tail + 4294967296 + 4 - head = tail + (4294967296 - head) + 4 := by omega
  rw [h]

/-- Exact round-trip for every valid/boundary reply length 1..1024 and every 32-bit head. -/
theorem msgqSz_roundtrip (head bytes : Nat)
    (hh : head < U32_MOD) (hb1 : 1 ≤ bytes) (hb2 : bytes ≤ 1024) :
    msgqSz head (simTail head bytes) = bytes := by
  unfold msgqSz simTail U32_MOD at *
  omega

/-- bytes = 4 always yields tail = head (indistinguishable from empty queue). -/
theorem simTail_four_eq_head (head : Nat) (hh : head < U32_MOD) :
    simTail head 4 = head := by
  unfold simTail U32_MOD at *
  omega

/-- bytes = 3 and every bytes in 5..1024 ALWAYS produce tail != head (triggers MSGQ poll). -/
theorem simTail_ne_head_boundaries (head bytes : Nat)
    (hh : head < U32_MOD)
    (hb : bytes = 3 ∨ (5 ≤ bytes ∧ bytes ≤ 1024)) :
    simTail head bytes ≠ head := by
  unfold simTail U32_MOD at *
  omega

/-- om/boot.om:253 admission `4 <= sz <= 1024` guarantees 1..256 EMEM dwords (fits 1024 B channel). -/
def admitPacketSize (sz : Nat) : Prop := 4 ≤ sz ∧ sz ≤ 1024

theorem admitPacketSize_emem_bounds (sz : Nat) (h : admitPacketSize sz) :
    1 ≤ sz / 4 ∧ sz / 4 ≤ 256 := by
  rcases h with ⟨h1, h2⟩; omega

/-- om/boot.om:226 admission `20 <= sz <= 1024` guarantees all 5 response dwords (0..4) were read. -/
def admitResponseSize (sz : Nat) : Prop := 20 ≤ sz ∧ sz ≤ 1024

theorem admitResponseSize_five_dwords (sz : Nat) (h : admitResponseSize sz) :
    5 ≤ sz / 4 ∧ sz / 4 ≤ 256 := by
  rcases h with ⟨h1, h2⟩; omega

/-- Negative control (Finding 1 mutant `lt sz 1`): admitting sz = 3 allows 0 dwords (premature ack). -/
def brokenAdmitPacketSize (sz : Nat) : Prop := 1 ≤ sz ∧ sz ≤ 1024

theorem neg_broken_admit_allows_zero_dwords :
    ∃ sz, brokenAdmitPacketSize sz ∧ sz / 4 = 0 := by
  exact ⟨3, ⟨by omega, by omega⟩, by omega⟩

/-! ## 2. SEC2 EMEM auto-increment encoding (om/boot.om:ememdw, gb20b/dev_sec_pri.h) -/

def AINCW : Nat := 16777216  -- 2^24
def AINCR : Nat := 33554432  -- 2^25

def ememEnc (dw : Nat) : Nat :=
  (dw / 64) * 256 + (dw % 64) * 4

/-- Exact model of om/boot.om `ememdw`: `add mul and shr v 8 255 64 and shr v 2 63` -/
def ememDec (v : Nat) : Nat :=
  ((v / 256) % 256) * 64 + ((v / 4) % 64)

theorem emem_roundtrip_plain (dw : Nat) (hd : dw ≤ 256) :
    ememDec (ememEnc dw) = dw := by
  unfold ememDec ememEnc; omega

theorem emem_roundtrip_aincw (dw : Nat) (hd : dw ≤ 256) :
    ememDec (AINCW + ememEnc dw) = dw := by
  unfold ememDec ememEnc AINCW; omega

theorem emem_roundtrip_aincr (dw : Nat) (hd : dw ≤ 256) :
    ememDec (AINCR + ememEnc dw) = dw := by
  unfold ememDec ememEnc AINCR; omega

/-! ## 3. GB20B Work-Submit Doorbell Token (om/rmarg.om op 4, gb202/dev_vm.h:27-32) -/

def DOORBELL_ENABLE : Nat := 1073741824  -- 2^30 (Wolfram verified)

/-- Exact arithmetic form of GB20B token: bit 30 | (rl << 16) | ch -/
def gb20bToken (rl ch : Nat) : Nat :=
  DOORBELL_ENABLE + rl * 65536 + ch

/-- Helper: if `b < 2^n`, then `(k * 2^n) ||| b = k * 2^n + b` (disjoint bit ranges). -/
theorem mul_two_pow_or_eq_add (k n b : Nat) (hb : b < 2^n) :
    (k * 2^n) ||| b = k * 2^n + b := by
  apply Nat.eq_of_testBit_eq
  intro i
  rw [Nat.testBit_or]
  by_cases hi : i < n
  · have hpow : 2^n = 2^(n - i) * 2^i := by
      rw [← Nat.pow_add]; congr 1; omega
    have hpow2 : 2^(n - i) = 2 * 2^(n - i - 1) := by
      have : n - i = (n - i - 1) + 1 := by omega
      conv => lhs; rw [this, Nat.pow_succ, Nat.mul_comm]
    have hdiv_lhs : (k * 2^n) / 2^i = 2 * (k * 2^(n - i - 1)) := by
      rw [hpow, ← Nat.mul_assoc, Nat.mul_div_cancel _ (Nat.two_pow_pos i), hpow2]
      rw [Nat.mul_left_comm]
    have hdiv_sum : (k * 2^n + b) / 2^i = 2 * (k * 2^(n - i - 1)) + b / 2^i := by
      rw [hpow, ← Nat.mul_assoc, Nat.mul_comm (k * 2^(n - i)) (2^i),
          Nat.add_comm, Nat.add_mul_div_left _ _ (Nat.two_pow_pos i), hpow2]
      rw [Nat.mul_left_comm, Nat.add_comm]
    have htb_lhs : (k * 2^n).testBit i = false := by
      rw [Nat.testBit_eq_decide_div_mod_eq, hdiv_lhs]; simp
    have htb_sum : (k * 2^n + b).testBit i = b.testBit i := by
      rw [Nat.testBit_eq_decide_div_mod_eq, Nat.testBit_eq_decide_div_mod_eq, hdiv_sum]
      simp [Nat.add_mod]
    simp [htb_lhs, htb_sum]
  · have hble : 2^n ≤ 2^i := Nat.pow_le_pow_right (by omega) (by omega)
    have hblt : b < 2^i := Nat.lt_of_lt_of_le hb hble
    have htb_b : b.testBit i = false := Nat.testBit_lt_two_pow hblt
    have hpow : 2^i = 2^n * 2^(i - n) := by
      rw [← Nat.pow_add]; congr 1; omega
    have hdiv_eq : (k * 2^n + b) / 2^i = (k * 2^n) / 2^i := by
      rw [hpow, ← Nat.div_div_eq_div_mul, ← Nat.div_div_eq_div_mul]
      have hinner : (k * 2^n + b) / 2^n = k := by
        rw [Nat.add_comm, Nat.add_mul_div_right _ _ (Nat.two_pow_pos n), Nat.div_eq_of_lt hb]
        simp
      rw [hinner, Nat.mul_div_cancel _ (Nat.two_pow_pos n)]
    have htb_sum : (k * 2^n + b).testBit i = (k * 2^n).testBit i := by
      rw [Nat.testBit_eq_decide_div_mod_eq, Nat.testBit_eq_decide_div_mod_eq, hdiv_eq]
    simp [htb_b, htb_sum]

/-- Exact bitwise OR model matching om/rmarg.om op 4: `or or shl rl 16 ch 1073741824`. -/
theorem gb20bToken_or_eq_add (rl ch : Nat) (hrl : rl ≤ 127) (hch : ch ≤ 4095) :
    ((rl <<< 16) ||| ch ||| DOORBELL_ENABLE) = gb20bToken rl ch := by
  unfold gb20bToken DOORBELL_ENABLE
  rw [Nat.shiftLeft_eq]
  have h1 : (rl * 2^16) ||| ch = rl * 2^16 + ch :=
    mul_two_pow_or_eq_add rl 16 ch (by omega)
  rw [h1, Nat.or_comm]
  have h2 : (1 * 2^30) ||| (rl * 2^16 + ch) = 1 * 2^30 + (rl * 2^16 + ch) :=
    mul_two_pow_or_eq_add 1 30 (rl * 2^16 + ch) (by omega)
  have h30 : (1073741824 : Nat) = 1 * 2^30 := by decide
  have h16 : (2 : Nat)^16 = 65536 := by decide
  rw [h30, h2, h16]
  omega

theorem gb20bToken_decode_exact (rl ch : Nat) (hrl : rl ≤ 127) (hch : ch ≤ 4095) :
    (gb20bToken rl ch) % 4096 = ch ∧
    ((gb20bToken rl ch) / 65536) % 128 = rl ∧
    ((gb20bToken rl ch) / 1073741824) % 2 = 1 ∧
    gb20bToken rl ch < U32_MOD := by
  unfold gb20bToken DOORBELL_ENABLE U32_MOD
  omega

theorem gb20bToken_injective (rl1 ch1 rl2 ch2 : Nat)
    (_h1 : rl1 ≤ 127) (h2 : ch1 ≤ 4095) (_h3 : rl2 ≤ 127) (h4 : ch2 ≤ 4095)
    (heq : gb20bToken rl1 ch1 = gb20bToken rl2 ch2) :
    rl1 = rl2 ∧ ch1 = ch2 := by
  unfold gb20bToken DOORBELL_ENABLE at heq
  omega

/-- Negative control: GB100/GB10B token (without bit 30) ALWAYS leaves GB20B doorbell disabled (bit 30 = 0). -/
def gb10bToken (rl ch : Nat) : Nat := rl * 65536 + ch

theorem neg_gb10bToken_disabled_on_gb20b (rl ch : Nat) (hrl : rl ≤ 127) (hch : ch ≤ 4095) :
    (gb10bToken rl ch / 1073741824) % 2 = 0 := by
  unfold gb10bToken; omega

/-! ## 4. GB20B Boot Protocol Ordering Invariant (State Machine Induction) -/

inductive Phase where
  | start | secboot | sendCot | waitReply | readReply | ackTail | ackHead | gspWait | done | stopped
  deriving DecidableEq

structure TraceState where
  phase      : Phase
  tailAcked  : Bool
  headAcked  : Bool
  ackOrderOk : Bool
  gspTouched : Bool
  replyOk    : Bool

def initState : TraceState :=
  { phase := .start, tailAcked := false, headAcked := false,
    ackOrderOk := true, gspTouched := false, replyOk := false }

inductive Step : TraceState → TraceState → Prop where
  | begin_secboot {s : TraceState} (hp : s.phase = .start) :
      Step s { s with phase := .secboot }
  | secboot_ok {s : TraceState} (hp : s.phase = .secboot) :
      Step s { s with phase := .sendCot }
  | cot_sent {s : TraceState} (hp : s.phase = .sendCot) :
      Step s { s with phase := .waitReply }
  | reply_size_ok {s : TraceState} (sz : Nat) (hp : s.phase = .waitReply) (hsz : admitPacketSize sz) :
      Step s { s with phase := .readReply }
  | reply_size_bad {s : TraceState} (sz : Nat) (hp : s.phase = .waitReply) (hsz : ¬ admitPacketSize sz) :
      Step s { s with phase := .stopped }
  | do_ack_tail {s : TraceState} (hp : s.phase = .readReply) :
      Step s { s with phase := .ackTail, tailAcked := true, ackOrderOk := s.ackOrderOk && !s.headAcked }
  | do_ack_head {s : TraceState} (hp : s.phase = .ackTail) :
      Step s { s with phase := .ackHead, headAcked := true }
  | reply_valid {s : TraceState} (sz : Nat) (hp : s.phase = .ackHead) (hsz : admitResponseSize sz) :
      Step s { s with phase := .gspWait, replyOk := true }
  | reply_invalid {s : TraceState} (hp : s.phase = .ackHead) :
      Step s { s with phase := .stopped }
  | gsp_access {s : TraceState} (hp : s.phase = .gspWait) :
      Step s { s with gspTouched := true }
  | finish_ok {s : TraceState} (hp : s.phase = .gspWait) :
      Step s { s with phase := .done, gspTouched := true }
  | fail_any {s : TraceState} :
      Step s { s with phase := .stopped }

inductive Reachable : TraceState → Prop where
  | init : Reachable initState
  | step {s s'} : Reachable s → Step s s' → Reachable s'

/-- Safety invariant preserved across every transition of the GB20B boot model:
    1. TAIL is always written strictly before HEAD (`ackOrderOk = true`).
    2. HEAD is never acknowledged without TAIL already acknowledged.
    3. No GSP register is ever accessed unless BOTH TAIL and HEAD were acknowledged AND reply was valid.
    4. Before `readReply`, neither TAIL nor HEAD was acknowledged (so `sz < 4` or `sz > 1024` never acks). -/
def SafeInv (s : TraceState) : Prop :=
  s.ackOrderOk = true ∧
  (s.headAcked = true → s.tailAcked = true) ∧
  (s.phase = .gspWait ∨ s.phase = .done → s.tailAcked = true ∧ s.headAcked = true ∧ s.replyOk = true) ∧
  (s.gspTouched = true → s.tailAcked = true ∧ s.headAcked = true ∧ s.replyOk = true) ∧
  (s.phase = .start ∨ s.phase = .secboot ∨ s.phase = .sendCot ∨ s.phase = .waitReply ∨ s.phase = .readReply →
     s.tailAcked = false ∧ s.headAcked = false) ∧
  (s.phase = .ackTail → s.tailAcked = true ∧ s.headAcked = false) ∧
  (s.phase = .ackHead → s.tailAcked = true ∧ s.headAcked = true)

theorem safeInv_init : SafeInv initState := by
  simp [SafeInv, initState]

theorem safeInv_step {s s'} (hinv : SafeInv s) (hst : Step s s') : SafeInv s' := by
  rcases hinv with ⟨h1, h2, h3, h4, h5, h6, h7⟩
  cases hst with
  | begin_secboot hp =>
    have hpre := h5 (Or.inl hp)
    refine ⟨h1, h2, by simp, h4, by simp [hpre], by simp, by simp⟩
  | secboot_ok hp =>
    have hpre := h5 (Or.inr (Or.inl hp))
    refine ⟨h1, h2, by simp, h4, by simp [hpre], by simp, by simp⟩
  | cot_sent hp =>
    have hpre := h5 (Or.inr (Or.inr (Or.inl hp)))
    refine ⟨h1, h2, by simp, h4, by simp [hpre], by simp, by simp⟩
  | reply_size_ok _ hp _ =>
    have hpre := h5 (Or.inr (Or.inr (Or.inr (Or.inl hp))))
    refine ⟨h1, h2, by simp, h4, by simp [hpre], by simp, by simp⟩
  | reply_size_bad _ _ _ =>
    refine ⟨h1, h2, by simp, h4, by simp, by simp, by simp⟩
  | do_ack_tail hp =>
    have hpre := h5 (Or.inr (Or.inr (Or.inr (Or.inr hp))))
    refine ⟨by simp [h1, hpre.2], by simp, by simp,
            by intro hg; exact ⟨rfl, (h4 hg).2.1, (h4 hg).2.2⟩,
            by simp, by simp [hpre.2], by simp⟩
  | do_ack_head hp =>
    have hat := h6 hp
    refine ⟨h1, by simp [hat.1], by simp,
            by intro hg; exact ⟨(h4 hg).1, rfl, (h4 hg).2.2⟩,
            by simp, by simp, by simp [hat.1]⟩
  | reply_valid _ hp _ =>
    have hah := h7 hp
    refine ⟨h1, h2, by simp [hah.1, hah.2],
            by intro _; exact ⟨hah.1, hah.2, rfl⟩,
            by simp, by simp, by simp⟩
  | reply_invalid _ =>
    refine ⟨h1, h2, by simp, h4, by simp, by simp, by simp⟩
  | gsp_access hp =>
    have hg := h3 (Or.inl hp)
    refine ⟨h1, h2, h3, by intro _; exact hg, by simp [hp], by simp [hp], by simp [hp]⟩
  | finish_ok hp =>
    have hg := h3 (Or.inl hp)
    refine ⟨h1, h2, by simp [hg], by intro _; exact hg, by simp, by simp, by simp⟩
  | fail_any =>
    refine ⟨h1, h2, by simp, h4, by simp, by simp, by simp⟩

theorem safeInv_reachable {s : TraceState} (hr : Reachable s) : SafeInv s := by
  induction hr with
  | init => exact safeInv_init
  | step _ hst ih => exact safeInv_step ih hst

/-- Negative control: a GB10B-style shortcut jumping from sendCot straight to gspWait violates SafeInv. -/
theorem neg_skip_reply_violates_safeInv :
    let bad : TraceState := { initState with phase := .gspWait, gspTouched := true }
    ¬ SafeInv bad := by
  intro bad h
  have := h.2.2.2.1 rfl
  simp [bad, initState] at this

/-! ## 5. COTv2 860-byte packed layout & WprMeta CrashCat disjointness (tools/gb20b_gsp_boot.c) -/

/-- Interval `[lo, hi)` disjointness check -/
def ivDisjoint (a b : Nat × Nat) : Bool :=
  a.2 ≤ b.1 || b.2 ≤ a.1

/-- NVDM_PAYLOAD_COT packed fields inside [0, 860) -/
def cotFields : List (Nat × Nat) :=
  [(0, 2), (2, 4), (4, 12), (12, 36), (36, 84), (84, 468), (468, 852), (852, 860)]

/-- WprMeta populated fields + GB20B CrashCat [224,232) and [232,236) inside [0, 256) -/
def wprFields : List (Nat × Nat) :=
  [(0, 88), (128, 136), (224, 232), (232, 236)]

def allPairwiseDisjoint : List (Nat × Nat) → Bool
  | [] => true
  | x :: xs => xs.all (ivDisjoint x) && allPairwiseDisjoint xs

def allValidIn (bound : Nat) (fs : List (Nat × Nat)) : Bool :=
  fs.all (fun p => p.1 < p.2 && p.2 ≤ bound)

theorem cotFields_disjoint_and_partition :
    allPairwiseDisjoint cotFields = true ∧
    allValidIn 860 cotFields = true ∧
    (cotFields.map (fun p => p.2 - p.1)).sum = 860 := by
  decide

theorem wprFields_crashcat_disjoint :
    allPairwiseDisjoint wprFields = true ∧
    allValidIn 256 wprFields = true := by
  decide

end AienGpuGb20b

#print axioms AienGpuGb20b.msgqSz_no_underflow
#print axioms AienGpuGb20b.msgqSz_eq_mod_sub_add4
#print axioms AienGpuGb20b.msgqSz_roundtrip
#print axioms AienGpuGb20b.simTail_four_eq_head
#print axioms AienGpuGb20b.simTail_ne_head_boundaries
#print axioms AienGpuGb20b.admitPacketSize_emem_bounds
#print axioms AienGpuGb20b.admitResponseSize_five_dwords
#print axioms AienGpuGb20b.neg_broken_admit_allows_zero_dwords
#print axioms AienGpuGb20b.emem_roundtrip_plain
#print axioms AienGpuGb20b.emem_roundtrip_aincw
#print axioms AienGpuGb20b.emem_roundtrip_aincr
#print axioms AienGpuGb20b.gb20bToken_or_eq_add
#print axioms AienGpuGb20b.gb20bToken_decode_exact
#print axioms AienGpuGb20b.gb20bToken_injective
#print axioms AienGpuGb20b.neg_gb10bToken_disabled_on_gb20b
#print axioms AienGpuGb20b.safeInv_reachable
#print axioms AienGpuGb20b.neg_skip_reply_violates_safeInv
#print axioms AienGpuGb20b.cotFields_disjoint_and_partition
#print axioms AienGpuGb20b.wprFields_crashcat_disjoint
