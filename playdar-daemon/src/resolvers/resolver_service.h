#ifndef __RESOLVER_SERVICE_H__
#define __RESOLVER_SERVICE_H__
// Interface for all resolver services
#include "resolvers/resolver.h"
#include "application/application.h"

#include <DynamicClass.hpp>


class ResolverService : public PDL::DynamicClass, std::exception


{
public:
    ResolverService(){}
    
    
    virtual void init(MyApplication * a)
    {
        m_app = a;
    }
    

    virtual std::string name() = 0;

    virtual void start_resolving(boost::shared_ptr<ResolverQuery> rq) = 0;

    virtual bool report_results(query_uid qid, vector< boost::shared_ptr<PlayableItem> > results);
    
//protected:
    MyApplication * app() { return m_app; }
    
    DECLARE_DYNAMIC_CLASS( ResolverService )
    
protected:
    virtual ~ResolverService() throw() {  }
    MyApplication * m_app;
};
#endif
