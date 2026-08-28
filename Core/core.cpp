#include "core.h"

Core& Core::instance()
{
    static Core inst; // 建立唯一的靜態實例
    return inst;      // 回傳該實例的引用
}

Core::~Core()
{
    // 釋放 Manager 物件 (這會進一步觸發 Manager 的解構式停止執行緒)
    if (m_manager) {
        delete m_manager;
        m_manager = nullptr;
    }
    // 釋放 TdProxy 物件
    if (m_proxy) {
        delete m_proxy;
        m_proxy = nullptr;
    }
}

void Core::init()
{
    m_proxy = new TaidaFlowProxy(this);
    m_manager = new Manager(this);

    //QObject::connect(m_proxy, &TaidaFlowProxy::setM1ValueSv, m_manager, &Manager::set_M1_SV);
    //QObject::connect(m_proxy, &TaidaFlowProxy::setM2ValueSv, m_manager, &Manager::set_M2_SV);
    //QObject::connect(m_proxy, &TaidaFlowProxy::setM3ValueSv, m_manager, &Manager::set_M3_SV);
    //QObject::connect(m_proxy, &TaidaFlowProxy::setM4ValueSv, m_manager, &Manager::set_M4_SV);

}