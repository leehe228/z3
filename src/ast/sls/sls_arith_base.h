/*++
Copyright (c) 2020 Microsoft Corporation

Module Name:

    sls_arith_base.h

Abstract:

    Theory plugin for arithmetic local search

Author:

    Nikolaj Bjorner (nbjorner) 2020-09-08

--*/
#pragma once

#include "util/obj_pair_set.h"
#include "util/checked_int64.h"
#include "util/optional.h"
#include "ast/ast_trail.h"
#include "ast/arith_decl_plugin.h"
#include "ast/sls/sls_context.h"
#include "ast/sls/sls_arith_clausal.h"

namespace sls {

    using theory_var = int;

    enum arith_move_type {
        hillclimb,
        hillclimb_plateau,
        random_update,
        random_inc_dec
    };

    std::ostream& operator<<(std::ostream& out, arith_move_type mt);

    static const unsigned null_arith_var = UINT_MAX;

    // local search portion for arithmetic
    template<typename num_t>
    class arith_base : public plugin {
        enum class ineq_kind { EQ, LE, LT};
        enum class var_sort { INT, REAL };
        struct bound { bool is_strict = false; num_t value; };
        typedef unsigned var_t;
        typedef unsigned atom_t;

        struct config {
            bool config_initialized = false;
            double cb = 2.85;
            unsigned L = 20;
            unsigned t = 45;
            unsigned max_no_improve = 500000;
            double sp = 0.0003;
            unsigned paws_init = 40;
            unsigned paws_sp = 52;
            bool paws = true;
            unsigned max_moves = 500;
            unsigned max_moves_base = 500;
            unsigned wp = 100;
            bool ucb = true;
            double ucb_constant = 1.0;
            double ucb_forget = 0.1;
            bool ucb_init = false;
            double ucb_noise = 0.1;
            unsigned restart_base = 1000;
            unsigned restart_next = 1000;
            unsigned restart_init = 1000;
            bool     use_lookahead = false;
            bool     use_clausal_lookahead = false;
            bool     allow_plateau = false;
        };

        struct stats {
            unsigned m_steps = 0;
            unsigned m_restarts = 0;
            unsigned m_propagations = 0;
        };

    public:
        struct linear_term {
            vector<std::pair<num_t, var_t>> m_args;
            num_t      m_coeff{ 0 };
        };
        struct nonlinear_coeff {
            var_t v;     // variable or multiplier containing x
            num_t coeff; // coeff of v in inequality
            unsigned p;  // power
        };

        typedef svector<std::pair<unsigned, unsigned>> monomial_t;

        // encode args <= bound, args = bound, args < bound
        struct ineq : public linear_term {    
            vector<std::pair<var_t, vector<nonlinear_coeff>>> m_nonlinear;
            vector<monomial_t> m_monomials;
            ineq_kind  m_op = ineq_kind::LE;            
            num_t      m_args_value;
            bool       m_is_linear = true;

            bool is_true() const;
            std::ostream& display(std::ostream& out) const;
        };
    private:

        class var_info {
            num_t        m_range{ 100000000 };
            unsigned     m_num_out_of_range = 0;
            unsigned     m_num_in_range = 0;
            num_t        m_value{ 0 };
            num_t        m_best_value{ 0 };
        public:
            var_info(expr* e, var_sort k): m_expr(e), m_sort(k) {}
            expr*        m_expr;

            var_sort     m_sort;
            arith_op_kind m_op = arith_op_kind::LAST_ARITH_OP;
            unsigned     m_def_idx = UINT_MAX;
            vector<std::pair<num_t, sat::bool_var>> m_linear_occurs;
            sat::bool_var_vector m_bool_vars_of;
            unsigned_vector m_clauses_of;
            unsigned_vector m_muls, m_adds, m_ops;
            optional<bound> m_lo, m_hi;
            vector<num_t> m_finite_domain;

            num_t const& value() const { return m_value; }
            void set_value(num_t const& v) { m_value = v; }

            num_t const& best_value() const { return m_best_value; }
            void set_best_value(num_t const& v) { m_best_value = v; }

            bool in_range(num_t const& n) {
                if (-m_range < n && n < m_range)
                    return true;
                bool result = false;
                if (m_lo)
                    result = n < m_lo->value + m_range;
                if (!result && m_hi)
                    result = n > m_hi->value - m_range;
#if 0
                if (!result) 
                    out_of_range();
                else 
                    ++m_num_in_range;
#endif
                return result;                
            }
            unsigned m_tabu_pos = 0, m_tabu_neg = 0;
            unsigned m_last_pos = 0, m_last_neg = 0;
            bool is_tabu(unsigned step, num_t const& delta) {
                return (delta > 0 ? m_tabu_pos : m_tabu_neg) > step;
            }
            void set_step(unsigned step, unsigned tabu_step, num_t const& delta) {
                if (delta > 0)
                    m_tabu_pos = tabu_step, m_last_pos = step;
                else
                    m_tabu_neg = tabu_step, m_last_neg = step;
            }
            unsigned last_step(num_t const& delta) const {
                return delta > 0 ? m_last_pos : m_last_neg;
            }
            void out_of_range() {
                ++m_num_out_of_range;
                if (m_num_out_of_range < 1000 * (1 + m_num_in_range))
                    return;
                IF_VERBOSE(2, verbose_stream() << "increase range " << m_range << "\n");
                m_range *= 2;
                m_num_out_of_range = 0;
                m_num_in_range = 0;
            }
        };

        struct mul_def {
            unsigned        m_var;
            monomial_t      m_monomial;
        };

        struct add_def : public linear_term {
            unsigned        m_var;
        };

        struct op_def {
            unsigned m_var = UINT_MAX;
            arith_op_kind m_op = LAST_ARITH_OP;
            unsigned m_arg1, m_arg2;
        };

        struct var_change {
            unsigned m_var;
            num_t    m_delta;
            double   m_score;
        };
       
        stats                        m_stats;
        config                       m_config;
        scoped_ptr_vector<ineq>      m_ineqs;
        vector<var_info>             m_vars;
        vector<mul_def>              m_muls;
        vector<add_def>              m_adds;
        vector<op_def>               m_ops;
        expr_ref_vector              m_new_terms;
        unsigned_vector              m_expr2var;
        svector<double>              m_probs;
        bool                         m_dscore_mode = false;
        vector<var_change>           m_updates;
        var_t                        m_last_var = 0;
        sat::literal                 m_last_literal = sat::null_literal;
        num_t                        m_last_delta { 0 };
        bool                         m_use_tabu = true;
        unsigned                     m_updates_max_size = 45;
        arith_util                   a;
        friend class arith_clausal<num_t>;
        arith_clausal<num_t>         m_clausal_sls;
        svector<double>              m_prob_break;
        indexed_uint_set             m_bool_var_atoms;
        indexed_uint_set             m_tmp_set;
        nat_set  m_tmp_nat_set;

        void invariant();
        void invariant(ineq const& i);

        unsigned get_num_vars() const { return m_vars.size(); }

        void updt_params();
        bool is_distinct(expr* e);
        bool eval_distinct(expr* e);
        void repair_distinct(expr* e);
        bool eval_is_correct(var_t v);      
        bool repair_mul(mul_def const& md);
        bool repair_add(add_def const& ad);
        bool repair_mod(op_def const& od);
        bool repair_idiv(op_def const& od);
        bool repair_div(op_def const& od);
        bool repair_rem(op_def const& od);
        bool repair_power(op_def const& od);
        bool repair_abs(op_def const& od);
        bool repair_to_int(op_def const& od);
        bool repair_to_real(op_def const& od);
        bool repair(sat::literal lit);
        bool in_bounds(var_t v, num_t const& value);
        bool is_fixed(var_t v);
        bool is_linear(var_t x, vector<nonlinear_coeff> const& nlc, num_t& b);
        bool is_quadratic(var_t x, vector<nonlinear_coeff> const& nlc, num_t& a, num_t& b);
        num_t mul_value_without(var_t m, var_t x);

        void add_update(var_t v, num_t delta);
        bool is_permitted_update(var_t v, num_t const& delta, num_t& delta_out);


        num_t value1(var_t v);

        vector<num_t> m_factors;
        vector<num_t> const& factor(num_t n);
        num_t root_of(unsigned n, num_t a);
        num_t power_of(num_t a, unsigned k);

        struct monomial_elem {
            num_t other_product;
            var_t v;
            unsigned p; // power
        };

        // double reward(sat::literal lit);

        bool sign(sat::bool_var v) const { return !ctx.is_true(sat::literal(v, false)); }
        ineq* get_ineq(sat::bool_var bv) const { return m_ineqs.get(bv, nullptr); }        
        num_t dtt(bool sign, ineq const& ineq) const { return dtt(sign, ineq.m_args_value, ineq); }
        num_t dtt(bool sign, num_t const& args_value, ineq const& ineq) const;
        num_t dtt(bool sign, ineq const& ineq, var_t v, num_t const& new_value) const;
        num_t dtt(bool sign, ineq const& ineq, num_t const& coeff, num_t const& delta) const;
        num_t dts(unsigned cl, var_t v, num_t const& new_value) const;
        num_t compute_dts(unsigned cl) const;

        bool is_mul(var_t v) const { return m_vars[v].m_op == arith_op_kind::OP_MUL; }
        bool is_add(var_t v) const { return m_vars[v].m_op == arith_op_kind::OP_ADD; }
        bool is_op(var_t v) const { return m_vars[v].m_op != arith_op_kind::LAST_ARITH_OP && m_vars[v].m_op != arith_op_kind::OP_MUL && m_vars[v].m_op != arith_op_kind::OP_ADD; }
        mul_def const& get_mul(var_t v) const { SASSERT(is_mul(v));  return m_muls[m_vars[v].m_def_idx]; }
        add_def const& get_add(var_t v) const { SASSERT(is_add(v));  return m_adds[m_vars[v].m_def_idx]; }

        bool update(var_t v, num_t const& new_value);
        bool apply_update();
        bool find_nl_moves(sat::literal lit);
        bool find_lin_moves(sat::literal lit);
        bool find_reset_moves(sat::literal lit);
        void add_reset_update(var_t v);
        void find_linear_moves(ineq const& i, var_t x, num_t const& coeff);
        void find_quadratic_moves(ineq const& i, var_t x, num_t const& a, num_t const& b, num_t const& sum);
        double compute_score(var_t x, num_t const& delta);
        void save_best_values();

        void initialize_bool_vars_of(var_t v);
        var_t mk_var(expr* e);
        var_t mk_term(expr* e);
        var_t mk_op(arith_op_kind k, expr* e, expr* x, expr* y);
        void add_arg(linear_term& term, num_t const& c, var_t v);
        void add_args(linear_term& term, expr* e, num_t const& sign);
        ineq& new_ineq(ineq_kind op, num_t const& bound);
        void init_ineq(sat::bool_var bv, ineq& i);
        void add_new_terms();
        num_t divide(var_t v, num_t const& delta, num_t const& coeff);
        num_t divide_floor(var_t v, num_t const& a, num_t const& b);
        num_t divide_ceil(var_t v, num_t const& a, num_t const& b);
        
        void init_bool_var_assignment(sat::bool_var v);

        bool is_int(var_t v) const { return m_vars[v].m_sort == var_sort::INT; }

        num_t value(var_t v) const { return m_vars[v].value(); }
        bool is_num(expr* e, num_t& i);
        num_t to_num(rational const& r);
        void check_real(expr* e);
        expr_ref from_num(sort* s, num_t const& n);
        void check_ineqs();
        void init_bool_var(sat::bool_var bv);
        void initialize_unit(sat::literal lit);
        void initialize_input_assertion(expr* f);
        void add_le(var_t v, num_t const& n);
        void add_ge(var_t v, num_t const& n);
        void add_lt(var_t v, num_t const& n);
        void add_gt(var_t v, num_t const& n);
        std::ostream& display(std::ostream& out, var_t v) const;
        std::ostream& display(std::ostream& out, add_def const& ad) const;
        std::ostream& display(std::ostream& out, mul_def const& md) const;



        // for global lookahead search mode
        void global_search();
        struct bool_info {
            unsigned weight = 0;
            double   score = 0;
            unsigned touched = 1;
            lbool    value = l_undef;
            sat::bool_var_vector fixable_atoms;
            svector<var_t>       fixable_vars;
            ptr_vector<expr>     fixable_exprs;
            bool_info(unsigned w) : weight(w) {}
        };

        vector<ptr_vector<app>> m_update_stack;
        expr_mark m_in_update_stack;
        svector<bool_info> m_bool_info;
        double m_best_score = 0, m_top_score = 0;
        unsigned m_min_depth = 0, m_max_depth = 0;
        num_t m_best_value;
        expr* m_best_expr = nullptr, * m_last_atom = nullptr, * m_last_expr = nullptr;
        expr_mark m_is_root;
        unsigned m_touched = 1;
        sat::bool_var_set m_fixed_atoms;
        uint64_t m_tabu_set = 0;
        unsigned m_global_search_count = 0;

        bool in_tabu_set(expr* e, num_t const& n);
        void insert_tabu_set(expr* e, num_t const& n);
        bool_info& get_bool_info(expr* e);
        bool get_bool_value(expr* e);
        bool get_bool_value_rec(expr* e);
        void set_bool_value(expr* e, bool v) { get_bool_info(e).value = to_lbool(v); }
        bool get_basic_bool_value(app* e);
        void initialize_bool_assignment();

        void finalize_bool_assignment();
        double old_score(expr* e) { return get_bool_info(e).score; }
        double new_score(expr* e);
        double new_score(expr* e, bool is_true);
        void set_score(expr* e, double s) { get_bool_info(e).score = s; }
        void rescore();
        void recalibrate_weights();
        void inc_weight(expr* e) { ++get_bool_info(e).weight;  }
        void dec_weight(expr* e) { auto& i = get_bool_info(e); i.weight = i.weight > m_config.paws_init ? i.weight - 1 : m_config.paws_init; }
        unsigned get_weight(expr* e) { return get_bool_info(e).weight; }
        unsigned get_touched(expr* e) { return get_bool_info(e).touched; }
        void inc_touched(expr* e) { ++get_bool_info(e).touched; }
        void set_touched(expr* e, unsigned t) { get_bool_info(e).touched = t; }
        void insert_update_stack(expr* t);
        void insert_update_stack_rec(expr* t);
        void clear_update_stack();
        void lookahead_num(var_t v, num_t const& value);
        bool can_update_num(var_t v, num_t const& delta);
        bool update_num(var_t v, num_t const& delta);
        void lookahead_bool(expr* e);
        double lookahead(expr* e, bool update_score);
        void add_lookahead(bool_info& i, expr* e);
        void add_lookahead(bool_info& i, sat::bool_var bv);
        ptr_vector<expr> const& get_fixable_exprs(expr* e);
        bool apply_move(expr* f, ptr_vector<expr> const& vars, arith_move_type t);
        expr* get_candidate_unsat();
        void check_restart();
        void ucb_forget();
        void update_args_value(var_t v, num_t const& new_value);
    public:
        arith_base(context& ctx);
        ~arith_base() override {}        
        void register_term(expr* e) override;
        bool set_value(expr* e, expr* v) override;
        expr_ref get_value(expr* e) override;
        void start_propagation() override;
        bool is_fixed(expr* e, expr_ref& value) override;
        void initialize() override;
        void propagate_literal(sat::literal lit) override;
        bool propagate() override;
        void repair_up(app* e) override;
        bool repair_down(app* e) override;
        void repair_literal(sat::literal lit) override;
        bool is_sat() override;
        void on_rescale() override;
        void on_restart() override;
        std::ostream& display(std::ostream& out) const override;
        void collect_statistics(statistics& st) const override;
        void reset_statistics() override;
    };


    inline std::ostream& operator<<(std::ostream& out, typename arith_base<checked_int64<true>>::ineq const& ineq) {
        return ineq.display(out);
    }

    inline std::ostream& operator<<(std::ostream& out, typename arith_base<rational>::ineq const& ineq) {
        return ineq.display(out);
    }
}
