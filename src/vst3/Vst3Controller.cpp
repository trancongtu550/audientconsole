#include "vst3/Vst3Controller.h"

#include "vst3/Vst3StateStream.h"

namespace audient::vst3
{

Vst3Controller::~Vst3Controller()
{
    close();
}

bool Vst3Controller::open(Steinberg::FUnknown* component, Steinberg::IPluginFactory* factory, Steinberg::FUnknown* hostContext)
{
    m_lastError.clear();
    close();

    if (component == nullptr || factory == nullptr)
    {
        m_lastError = "invalid component/factory";
        return false;
    }
    if (component->queryInterface(Steinberg::Vst::IComponent::iid.toTUID(), reinterpret_cast<void**>(&m_componentIf)) != Steinberg::kResultOk ||
        m_componentIf == nullptr)
    {
        m_lastError = "component has no IComponent";
        return false;
    }

    // Single-component plug-in: the controller IS the component. The component
    // was already initialized by Vst3Processor::prepare; do NOT initialize or
    // connect it again (reference host sets isSingleComponent and skips both).
    if (m_componentIf->queryInterface(Steinberg::Vst::IEditController::iid.toTUID(),
                                      reinterpret_cast<void**>(&m_controller)) == Steinberg::kResultOk &&
        m_controller != nullptr)
    {
        m_isControllerComponent = true;
        return true;
    }
    m_controller = nullptr;

    // Separate controller class: ask the component for its controller class id
    // and create it from the factory (reference host setupPlugin).
    Steinberg::TUID controllerCid{};
    const Steinberg::tresult cidResult = m_componentIf->getControllerClassId(controllerCid);
    if (cidResult != Steinberg::kResultOk && cidResult != Steinberg::kResultTrue)
    {
        m_lastError = "component does not provide a controller class id";
        return false;
    }
    bool emptyCid = true;
    for (const unsigned char byte : controllerCid)
    {
        if (byte != 0)
        {
            emptyCid = false;
            break;
        }
    }
    if (emptyCid)
    {
        m_lastError = "component returns an empty controller class id";
        return false;
    }

    void* controllerVoid = nullptr;
    if (factory->createInstance(controllerCid, Steinberg::Vst::IEditController::iid.toTUID(), &controllerVoid) != Steinberg::kResultOk ||
        controllerVoid == nullptr)
    {
        m_lastError = "factory could not create the editor controller";
        return false;
    }
    m_controller = reinterpret_cast<Steinberg::Vst::IEditController*>(controllerVoid);

    // Initialize the separate controller with our host context. The component
    // itself was already initialized by Vst3Processor::prepare.
    if (m_controller->initialize(hostContext) != Steinberg::kResultOk)
    {
        m_lastError = "controller initialize failed";
        close();
        return false;
    }

    // Connect component <-> controller through IConnectionPoint (reference host
    // connectComponents). Both sides must expose the interface for a separate
    // controller; the borrowed CP pointers belong to the component/controller
    // and are never released here.
    m_componentIf->queryInterface(Steinberg::Vst::IConnectionPoint::iid.toTUID(), reinterpret_cast<void**>(&m_componentCP));
    m_controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid.toTUID(), reinterpret_cast<void**>(&m_controllerCP));
    if (m_componentCP == nullptr || m_controllerCP == nullptr)
    {
        m_lastError = "separate controller requires IConnectionPoint on both sides";
        close();
        return false;
    }
    if (m_componentCP->connect(m_controllerCP) != Steinberg::kResultOk ||
        m_controllerCP->connect(m_componentCP) != Steinberg::kResultOk)
    {
        m_lastError = "component<->controller connect failed";
        // Mark connected so close() disconnects both peers even when only one
        // side of a two-way connect succeeded (asymmetric failure).
        m_connected = true;
        close();
        return false;
    }
    m_connected = true;
    return true;
}

void Vst3Controller::close()
{
    if (m_connected)
    {
        if (m_controllerCP != nullptr && m_componentCP != nullptr)
        {
            m_controllerCP->disconnect(m_componentCP);
            m_componentCP->disconnect(m_controllerCP);
        }
        m_connected = false;
    }
    if (m_controller != nullptr)
    {
        // Same-object controller's terminate belongs to the component owner
        // (Vst3Processor::teardown). The separate controller terminate + release
        // balances the createInstance ref the factory handed to us.
        if (!m_isControllerComponent)
        {
            m_controller->terminate();
            m_controller->release();
        }
        m_controller = nullptr;
    }
    m_componentIf = nullptr;
    m_componentCP = nullptr;
    m_controllerCP = nullptr;
    m_isControllerComponent = false;
}

bool Vst3Controller::isOpen() const
{
    return m_controller != nullptr;
}

bool Vst3Controller::isControllerComponent() const
{
    return m_isControllerComponent;
}

Steinberg::Vst::IEditController* Vst3Controller::controller() const
{
    return m_controller;
}

bool Vst3Controller::saveControllerState(std::vector<std::uint8_t>& outBlob)
{
    outBlob.clear();
    if (m_controller == nullptr || !isOpen())
    {
        m_lastError = "no controller open";
        return false;
    }

    Vst3StateStream stream;
    const Steinberg::tresult result = m_controller->getState(&stream);
    if (result != Steinberg::kResultOk || stream.overflow())
    {
        m_lastError = "controller getState failed or wrote more than kMaxStateBytes";
        return false;
    }

    outBlob = Vst3StateBlob::encode(stream.view());
    if (outBlob.empty())
    {
        m_lastError = "controller state blob encode failed (payload exceeds kMaxStateBytes)";
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Vst3Controller::restoreControllerState(const std::vector<std::uint8_t>& blob)
{
    if (m_controller == nullptr || !isOpen())
    {
        m_lastError = "no controller open";
        return false;
    }

    std::vector<std::uint8_t> payload;
    if (!Vst3StateBlob::decode(blob, payload))
    {
        m_lastError = "controller state blob rejected (magic/version/length/checksum)";
        return false;
    }

    Vst3StateStream stream;
    if (!payload.empty())
    {
        const Steinberg::int32 numBytes = static_cast<Steinberg::int32>(payload.size());
        if (stream.write(payload.data(), numBytes, nullptr) != Steinberg::kResultTrue)
        {
            m_lastError = "controller state payload cannot be staged for setState";
            return false;
        }
    }
    if (stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr) != Steinberg::kResultTrue)
    {
        m_lastError = "controller state payload cannot be rewound for setState";
        return false;
    }

    const Steinberg::tresult result = m_controller->setState(&stream);
    if (result != Steinberg::kResultOk)
    {
        m_lastError = "controller setState rejected the restored state";
        return false;
    }
    m_lastError.clear();
    return true;
}

const std::string& Vst3Controller::lastError() const
{
    return m_lastError;
}

} // namespace audient::vst3