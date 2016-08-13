/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/ufcs_everything.cpp
 * - Expand all function calls (_CallMethod, and _CallValue) and operator overloads to _CallPath
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace {
    
    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::Crate& m_crate;
        ::HIR::ExprNodeP    m_replacement;
        
    public:
        ExprVisitor_Mutate(const ::HIR::Crate& crate):
            m_crate(crate)
        {
        }
        void visit_node_ptr(::HIR::ExprPtr& root) {
            const auto& node_ref = *root;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*root << " " << node_ty << " : " << root->m_res_type, node_ty);
            root->visit(*this);
            if( m_replacement ) {
                root.reset( m_replacement.release() );
            }
        }
        
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*node << " " << node_ty << " : " << node->m_res_type, node_ty);
            assert( node );
            node->visit(*this);
            if( m_replacement ) {
                node = mv$(m_replacement);
            }
        }
        
        // ----------
        // _CallValue
        // ----------
        // Replace with a UFCS call using the now-known type
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            const auto& sp = node.span();
            
            ::HIR::ExprVisitorDef::visit(node);
            
            ::HIR::PathParams   trait_args;
            {
                ::std::vector< ::HIR::TypeRef>  arg_types;
                // NOTE: In this case, m_arg_types is just the argument types
                for(const auto& arg_ty : node.m_arg_types)
                    arg_types.push_back(arg_ty.clone());
                trait_args.m_types.push_back( ::HIR::TypeRef(mv$(arg_types)) );
            }
            
            // TODO: You can call via &-ptrs, but that currently isn't handled in typeck
            TU_IFLET(::HIR::TypeRef::Data, node.m_value->m_res_type.m_data, Closure, e,
                if( node.m_trait_used == ::HIR::ExprNode_CallValue::TraitUsed::Unknown )
                {
                    // NOTE: Closure node still exists, and will do until MIR construction deletes the HIR
                    switch(e.node->m_class)
                    {
                    case ::HIR::ExprNode_Closure::Class::Unknown:
                        BUG(sp, "References an ::Unknown closure");
                    case ::HIR::ExprNode_Closure::Class::NoCapture:
                    case ::HIR::ExprNode_Closure::Class::Shared:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                        // TODO: Add borrow.
                        break;
                    case ::HIR::ExprNode_Closure::Class::Mut:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnMut;
                        // TODO: Add borrow.
                        break;
                    case ::HIR::ExprNode_Closure::Class::Once:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnOnce;
                        // TODO: Add borrow.
                        break;
                    }
                }
            )
            
            // Use marking in node to determine trait to use
            ::HIR::Path   method_path(::HIR::SimplePath{});
            switch(node.m_trait_used)
            {
            case ::HIR::ExprNode_CallValue::TraitUsed::Fn:
                method_path = ::HIR::Path(
                    node.m_value->m_res_type.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn"), mv$(trait_args) ),
                    "call"
                    );
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnMut:
                method_path = ::HIR::Path(
                    node.m_value->m_res_type.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn_mut"), mv$(trait_args) ),
                    "call_mut"
                    );
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnOnce:
                method_path = ::HIR::Path(
                    node.m_value->m_res_type.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn_once"), mv$(trait_args) ),
                    "call_once"
                    );
                break;
            
            //case ::HIR::ExprNode_CallValue::TraitUsed::Unknown:
            default:
                BUG(node.span(), "Encountered CallValue with TraitUsed::Unknown, ty=" << node.m_value->m_res_type);
            }
            
            auto self_arg_type = node.m_value->m_res_type.clone();
            // Construct argument list for the output
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.reserve( 1 + node.m_args.size() );
            args.push_back( mv$(node.m_value) );
            for(auto& arg : node.m_args)
                args.push_back( mv$(arg) );
            
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                mv$(method_path),
                mv$(args)
                );
            
            // Populate the cache for later passes
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( mv$(self_arg_type) );
            for(auto& ty : node.m_arg_types)
                arg_types.push_back( mv$(ty) );
        }
        
        // ----------
        // _CallMethod
        // ----------
        // Simple replacement
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            const auto& sp = node.span();
            
            ::HIR::ExprVisitorDef::visit(node);
            
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.reserve( 1 + node.m_args.size() );
            args.push_back( mv$(node.m_value) );
            for(auto& arg : node.m_args)
                args.push_back( mv$(arg) );
            
            // Replace using known function path
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                mv$(node.m_method_path),
                mv$(args)
                );
            // Populate the cache for later passes
            dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache = mv$(node.m_cache);
        }
        
        
        static bool is_op_valid_shift(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r)
        {
            // Integer with any other integer is valid, others go to overload resolution
            if( ty_l.m_data.is_Primitive() && ty_r.m_data.is_Primitive() ) {
                switch(ty_l.m_data.as_Primitive())
                {
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Bool:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    break;
                default:
                    switch(ty_r.m_data.as_Primitive())
                    {
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        break;
                    default:
                        // RETURN early
                        return true;
                    }
                    break;
                }
                
            }
            return false;
        }
        static bool is_op_valid_bitmask(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r)
        {
            // Equal integers and bool are valid
            if( ty_l == ty_r ) {
                TU_IFLET(::HIR::TypeRef::Data, ty_l.m_data, Primitive, e,
                    switch(e)
                    {
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                        break;
                    default:
                        // RETURN early
                        return true;
                    }
                )
            }
            return false;
        }
        static bool is_op_valid_arith(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r)
        {
            // Equal floats/integers are valid, others go to overload
            if( ty_l == ty_r ) {
                TU_IFLET(::HIR::TypeRef::Data, ty_l.m_data, Primitive, e,
                    switch(e)
                    {
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Bool:
                        break;
                    default:
                        // RETURN early
                        return true;
                    }
                )
            }
            return false;
        }
        
        // -------
        // _Assign
        // -------
        // Replace with overload call if not a builtin supported operation
        void visit(::HIR::ExprNode_Assign& node) override
        {
            const auto& sp = node.span();
            ::HIR::ExprVisitorDef::visit(node);
            
            const auto& ty_slot = node.m_slot->m_res_type;
            const auto& ty_val  = node.m_value->m_res_type;
            
            const char* langitem = nullptr;
            const char* opname = nullptr;
            #define _(opname)   case ::HIR::ExprNode_Assign::Op::opname
            switch( node.m_op )
            {
            _(None):
                ASSERT_BUG(sp, ty_slot == ty_val, "Types must equal for non-operator assignment, " << ty_slot << " != " << ty_val);
                return ;
            _(Shr): {langitem = "shr_assign"; opname = "shr_assign"; } if(0)
            _(Shl): {langitem = "shl_assign"; opname = "shl_assign"; } if(0)
                ;
                if( is_op_valid_shift(ty_slot, ty_val) ) {
                    return ;
                }
                break;
            
            _(And): {langitem = "bitand_assign"; opname = "bitand_assign"; } if(0)
            _(Or ): {langitem = "bitor_assign" ; opname = "bitor_assign" ; } if(0)
            _(Xor): {langitem = "bitxor_assign"; opname = "bitxor_assign"; } if(0)
                ;
                if( is_op_valid_bitmask(ty_slot, ty_val) ) {
                    return ;
                }
                break;

            _(Add): {langitem = "add_assign"; opname = "add_assign"; } if(0)
            _(Sub): {langitem = "sub_assign"; opname = "sub_assign"; } if(0)
            _(Mul): {langitem = "mul_assign"; opname = "mul_assign"; } if(0)
            _(Div): {langitem = "div_assign"; opname = "div_assign"; } if(0)
            _(Mod): {langitem = "rem_assign"; opname = "rem_assign"; } if(0)
                ;
                if( is_op_valid_arith(ty_slot, ty_val) ) {
                    return ;
                }
                // - Fall down to overload replacement
                break;
            }
            #undef _
            assert( langitem );
            assert( opname );
            
            // Needs replacement, continue
            ::HIR::PathParams   trait_params;
            trait_params.m_types.push_back( ty_val.clone() );
            ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), mv$(trait_params) };
            
            auto slot_type_refmut = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, ty_slot.clone());
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.push_back( NEWNODE( slot_type_refmut.clone(), UniOp, sp,
                ::HIR::ExprNode_UniOp::Op::RefMut, mv$(node.m_slot)
                ) );
            args.push_back( mv$(node.m_value) );
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                ::HIR::Path(ty_slot.clone(), mv$(trait), opname),
                mv$(args)
                );
            
            // Populate the cache for later passes
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( mv$(slot_type_refmut) );
            arg_types.push_back( ty_val.clone() );
            arg_types.push_back( ::HIR::TypeRef::new_unit() );
        }
        
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            const auto& sp = node.span();
            ::HIR::ExprVisitorDef::visit(node);
            
            const auto& ty_l = node.m_left->m_res_type;
            const auto& ty_r  = node.m_right->m_res_type;
            
            const char* langitem = nullptr;
            const char* method = nullptr;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu: { langitem = "eq"; method = "eq"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:{ langitem = "eq"; method = "ne"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLt:  { langitem = "ord"; method = "lt"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLtE: { langitem = "ord"; method = "le"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGt:  { langitem = "ord"; method = "gt"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGtE: { langitem = "ord"; method = "ge"; } if(0)
                ; {
                // 1. Check if the types are valid for primitive comparison
                if( ty_l == ty_r ) {
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (ty_l.m_data), (e),
                    (
                        // Unknown - Overload
                        ),
                    (Pointer,
                        // Raw pointer, valid.
                        return ;
                        ),
                    // TODO: Should comparing &str be handled by the overload, or MIR?
                    (Primitive,
                        if( e != ::HIR::CoreType::Str ) {
                            return ;
                        }
                        )
                    )
                }
                // 2. If not, emit a call with params borrowed
                ::HIR::PathParams   trait_params;
                trait_params.m_types.push_back( ty_r.clone() );
                ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), mv$(trait_params) };
                
                auto ty_l_ref = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ty_l.clone() );
                auto ty_r_ref = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ty_r.clone() );
                
                ::std::vector< ::HIR::ExprNodeP>    args;
                args.push_back( NEWNODE(ty_l_ref.clone(), UniOp, node.m_left->span(),
                    ::HIR::ExprNode_UniOp::Op::Ref, mv$(node.m_left)
                    ) );
                args.push_back( NEWNODE(ty_r_ref.clone(), UniOp, node.m_right->span(),
                    ::HIR::ExprNode_UniOp::Op::Ref, mv$(node.m_right)
                    ) );
                
                m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                    ::HIR::Path(ty_l.clone(), mv$(trait), method),
                    mv$(args)
                    );
                
                // Populate the cache for later passes
                auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
                arg_types.push_back( mv$(ty_l_ref) );
                arg_types.push_back( mv$(ty_r_ref) );
                arg_types.push_back( ::HIR::TypeRef( ::HIR::CoreType::Bool ) );
                return ;
                } break;
            
            case ::HIR::ExprNode_BinOp::Op::Xor: langitem = method = "bitxor"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Or : langitem = method = "bitor" ; if(0)
            case ::HIR::ExprNode_BinOp::Op::And: langitem = method = "bitand"; if(0)
                ;
                if( is_op_valid_bitmask(ty_l, ty_r) ) {
                    return ;
                }
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Shr: langitem = method = "shr"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Shl: langitem = method = "shr";
                if( is_op_valid_shift(ty_l, ty_r) ) {
                    return ;
                }
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Add: langitem = method = "add"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Sub: langitem = method = "sub"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mul: langitem = method = "mul"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Div: langitem = method = "div"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mod: langitem = method = "rem";
                if( is_op_valid_arith(ty_l, ty_r) ) {
                    return ;
                }
                break;
            
            case ::HIR::ExprNode_BinOp::Op::BoolAnd:
            case ::HIR::ExprNode_BinOp::Op::BoolOr:
                ASSERT_BUG(sp, ty_l == ::HIR::TypeRef(::HIR::CoreType::Bool), "&& operator requires bool");
                ASSERT_BUG(sp, ty_r == ::HIR::TypeRef(::HIR::CoreType::Bool), "&& operator requires bool");
                return ;
            }
            assert(langitem);
            assert(method);
            
            // Needs replacement, continue
            ::HIR::PathParams   trait_params;
            trait_params.m_types.push_back( ty_r.clone() );
            ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), mv$(trait_params) };
            
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.push_back( mv$(node.m_left) );
            args.push_back( mv$(node.m_right) );
            
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                ::HIR::Path(ty_l.clone(), mv$(trait), method),
                mv$(args)
                );
            
            // Populate the cache for later passes
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( ty_l.clone() );
            arg_types.push_back( ty_r.clone() );
            arg_types.push_back( m_replacement->m_res_type.clone() );
        }
    };
    class OuterVisitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_crate(crate)
        {
        }
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                if( e.size ) {
                    ExprVisitor_Mutate  ev(m_crate);
                    ev.visit_node_ptr( e.size );
                }
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);
            if( item.m_code )
            {
                DEBUG("Function code " << p);
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr( item.m_code );
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    DEBUG("Enum value " << p << " - " << var.first);
                    
                    ExprVisitor_Mutate  ev(m_crate);
                    ev.visit_node_ptr(e);
                )
            }
        }
    };
}   // namespace

void HIR_Expand_UfcsEverything(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
