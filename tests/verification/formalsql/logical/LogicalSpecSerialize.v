(* Deterministic JSON serialization of RelNode LogicalSpec trees.
   Query-independent. Dump scripts Compute serialize strings; no hardcoded queries. *)

From Stdlib Require Import List ZArith String Nat.
Import ListNotations.
Local Open Scope string_scope.
Local Open Scope Z_scope.

From FormalSQLLogical Require Import SqlTypes Schema Expr Agg RelNode.

Definition digit_to_string (d : nat) : string :=
  match d with
  | 0%nat => "0" | 1%nat => "1" | 2%nat => "2" | 3%nat => "3" | 4%nat => "4"
  | 5%nat => "5" | 6%nat => "6" | 7%nat => "7" | 8%nat => "8" | _ => "9"
  end.

Fixpoint nat_to_string_aux (n fuel : nat) (acc : string) : string :=
  match fuel with
  | 0%nat => acc
  | S fuel' =>
      let d := Nat.modulo n 10 in
      let n' := Nat.div n 10 in
      let acc' := digit_to_string d ++ acc in
      if Nat.eqb n' 0 then acc' else nat_to_string_aux n' fuel' acc'
  end.

Definition nat_to_string (n : nat) : string :=
  if Nat.eqb n 0 then "0" else nat_to_string_aux n (S n) "".

Definition Z_to_string (z : Z) : string :=
  if Z.ltb z 0 then "-" ++ nat_to_string (Z.to_nat (- z))
  else nat_to_string (Z.to_nat z).

Definition comma_join (xs : list string) : string :=
  match xs with
  | [] => ""
  | x :: xs' => fold_left (fun acc y => acc ++ "," ++ y) xs' x
  end.

Definition list_to_json {A} (f : A -> string) (xs : list A) : string :=
  "[" ++ comma_join (List.map f xs) ++ "]".

Definition sql_type_to_json (t : SqlType) : string :=
  match t with
  | TInt => "{""kind"":""int""}"
  | TNumeric s => "{""kind"":""numeric"",""scale"":" ++ nat_to_string s ++ "}"
  | TBool => "{""kind"":""bool""}"
  | TDate => "{""kind"":""date""}"
  | TText => "{""kind"":""text""}"
  | TTyUnknown => "{""kind"":""unknown""}"
  end.

Definition sql_value_to_json (v : SqlValue) : string :=
  match v with
  | VNull => "{""kind"":""null""}"
  | VInt z => "{""kind"":""int"",""value"":" ++ Z_to_string z ++ "}"
  | VBool b => "{""kind"":""bool"",""value"":" ++ (if b then "true" else "false") ++ "}"
  | VText s => "{""kind"":""text"",""value"":""" ++ s ++ """}"
  | VAvgExact su co =>
      "{""kind"":""avg_exact"",""sum"":" ++ Z_to_string su
      ++ ",""count"":" ++ Z_to_string co ++ "}"
  end.

Definition unary_op_to_json (o : UnaryOp) : string :=
  match o with
  | UNeg => """neg""" | UNot => """not"""
  | UIsNull => """is_null""" | UIsNotNull => """is_not_null"""
  end.

Definition binary_op_to_json (o : BinaryOp) : string :=
  match o with
  | BAdd => """add""" | BSub => """sub""" | BMul => """mul""" | BDiv => """div"""
  | BEq => """eq""" | BNe => """ne""" | BLt => """lt""" | BLe => """le"""
  | BGt => """gt""" | BGe => """ge"""
  | BAnd => """and""" | BOr => """or"""
  | BLike => """like""" | BPrefixMatch => """prefix"""
  end.

Fixpoint expr_to_json (e : Expr) : string :=
  match e with
  | EInputRef i =>
      "{""kind"":""input_ref"",""slot"":" ++ nat_to_string i ++ "}"
  | EConst v ty =>
      "{""kind"":""const"",""value"":" ++ sql_value_to_json v
      ++ ",""ty"":" ++ sql_type_to_json ty ++ "}"
  | EParam id ty =>
      "{""kind"":""param"",""id"":""" ++ id ++ """"
      ++ ",""ty"":" ++ sql_type_to_json ty ++ "}"
  | EUnary o e1 =>
      "{""kind"":""unary"",""op"":" ++ unary_op_to_json o
      ++ ",""expr"":" ++ expr_to_json e1 ++ "}"
  | EBinary o l r =>
      "{""kind"":""binary"",""op"":" ++ binary_op_to_json o
      ++ ",""left"":" ++ expr_to_json l
      ++ ",""right"":" ++ expr_to_json r ++ "}"
  | ECase brs el =>
      let branch (p : Expr * Expr) :=
        "{""cond"":" ++ expr_to_json (fst p)
        ++ ",""then"":" ++ expr_to_json (snd p) ++ "}" in
      "{""kind"":""case"",""branches"":" ++ list_to_json branch brs
      ++ ",""else"":" ++ expr_to_json el ++ "}"
  | ECast e1 ty =>
      "{""kind"":""cast"",""expr"":" ++ expr_to_json e1
      ++ ",""ty"":" ++ sql_type_to_json ty ++ "}"
  | EGroupRef i =>
      "{""kind"":""group_ref"",""i"":" ++ nat_to_string i ++ "}"
  | EAggRef i =>
      "{""kind"":""agg_ref"",""i"":" ++ nat_to_string i ++ "}"
  | EFunction id args =>
      "{""kind"":""function"",""id"":""" ++ id ++ """"
      ++ ",""args"":" ++ list_to_json expr_to_json args ++ "}"
  end.

Definition agg_kind_to_json (k : AggKind) : string :=
  match k with
  | AggCountStar => """count_star"""
  | AggCount => """count"""
  | AggSum => """sum"""
  | AggAvg => """avg"""
  | AggMin => """min"""
  | AggMax => """max"""
  end.

Definition agg_to_json (a : AggSpec) : string :=
  match agg_expr a with
  | None =>
      "{""kind"":" ++ agg_kind_to_json (agg_kind a) ++ "}"
  | Some e =>
      "{""kind"":" ++ agg_kind_to_json (agg_kind a)
      ++ ",""expr"":" ++ expr_to_json e ++ "}"
  end.

Definition sort_dir_to_json (d : SortDir) : string :=
  match d with SortAsc => """asc""" | SortDesc => """desc""" end.

Definition null_order_to_json (n : NullOrder) : string :=
  match n with NullsFirst => """first""" | NullsLast => """last""" end.

Definition order_key_to_json (k : OrderKey) : string :=
  "{""expr"":" ++ expr_to_json (ok_expr k)
  ++ ",""dir"":" ++ sort_dir_to_json (ok_dir k)
  ++ ",""nulls"":" ++ null_order_to_json (ok_nulls k) ++ "}".

Definition join_kind_to_json (k : JoinKind) : string :=
  match k with
  | JoinInner => """inner""" | JoinLeft => """left""" | JoinRight => """right"""
  | JoinFull => """full""" | JoinCross => """cross"""
  end.

Fixpoint rel_to_json (n : RelNode) : string :=
  match n with
  | RelScan t =>
      "{""kind"":""scan"",""table"":""" ++ t ++ """}"
  | RelFilter inn pr =>
      "{""kind"":""filter"",""input"":" ++ rel_to_json inn
      ++ ",""pred"":" ++ expr_to_json pr ++ "}"
  | RelProject inn es =>
      "{""kind"":""project"",""input"":" ++ rel_to_json inn
      ++ ",""exprs"":" ++ list_to_json expr_to_json es ++ "}"
  | RelJoin k l r c =>
      "{""kind"":""join"",""join_kind"":" ++ join_kind_to_json k
      ++ ",""left"":" ++ rel_to_json l
      ++ ",""right"":" ++ rel_to_json r
      ++ ",""cond"":" ++ expr_to_json c ++ "}"
  | RelSemiJoin l r c =>
      "{""kind"":""semi_join"",""left"":" ++ rel_to_json l
      ++ ",""right"":" ++ rel_to_json r
      ++ ",""cond"":" ++ expr_to_json c ++ "}"
  | RelAntiJoin l r c =>
      "{""kind"":""anti_join"",""left"":" ++ rel_to_json l
      ++ ",""right"":" ++ rel_to_json r
      ++ ",""cond"":" ++ expr_to_json c ++ "}"
  | RelAggregate inn gs as_ =>
      "{""kind"":""aggregate"",""input"":" ++ rel_to_json inn
      ++ ",""group_exprs"":" ++ list_to_json expr_to_json gs
      ++ ",""aggs"":" ++ list_to_json agg_to_json as_ ++ "}"
  | RelSort inn oks =>
      "{""kind"":""sort"",""input"":" ++ rel_to_json inn
      ++ ",""order_keys"":" ++ list_to_json order_key_to_json oks ++ "}"
  | RelDistinct inn =>
      "{""kind"":""distinct"",""input"":" ++ rel_to_json inn ++ "}"
  | RelLimit inn off cnt =>
      "{""kind"":""limit"",""input"":" ++ rel_to_json inn
      ++ ",""offset"":" ++ nat_to_string off
      ++ ",""count"":" ++ nat_to_string cnt ++ "}"
  end.

Definition serialize_logical_spec_rel_json (s : LogicalSpec) : string :=
  "{""version"":""logical_spec_relnode_v1"",""tree"":" ++ rel_to_json s ++ "}".
