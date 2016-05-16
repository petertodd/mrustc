/*
 */
#include <hir/path.hpp>
#include <hir/type.hpp>

::HIR::SimplePath HIR::SimplePath::operator+(const ::std::string& s) const
{
    ::HIR::SimplePath ret(m_crate_name);
    ret.m_components = m_components;
    
    ret.m_components.push_back( s );
    
    return ret;
}
namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::SimplePath& x)
    {
        if( x.m_crate_name != "" ) {
            os << "::\"" << x.m_crate_name << "\"";
        }
        else if( x.m_components.size() == 0 ) {
            os << "::";
        }
        else {
        }
        for(const auto& n : x.m_components)
        {
            os << "::" << n;
        }
        return os;
    }
}

::HIR::PathParams::PathParams()
{
}

::HIR::GenericPath::GenericPath()
{
}
::HIR::GenericPath::GenericPath(::HIR::SimplePath sp):
    m_path( mv$(sp) )
{
}
::HIR::GenericPath::GenericPath(::HIR::SimplePath sp, ::HIR::PathParams params):
    m_path( mv$(sp) ),
    m_params( mv$(params) )
{
}


::HIR::Path::Path(::HIR::GenericPath gp):
    m_data( ::HIR::Path::Data::make_Generic( mv$(gp) ) )
{
}
::HIR::Path::Path(::HIR::TypeRefPtr type, ::HIR::GenericPath trait, ::std::string item, ::HIR::PathParams params):
    m_data( ::HIR::Path::Data::make_UFCS({
        mv$(type), mv$(trait), mv$(item), mv$(params)
        }) )
{
}
::HIR::Path::Path(::HIR::SimplePath sp):
    m_data( ::HIR::Path::Data::make_Generic(::HIR::GenericPath(mv$(sp))) )
{
}

