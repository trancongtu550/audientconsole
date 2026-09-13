#include "production/ChannelVstChain.h"

#include "vst3/Vst3Controller.h"
#include "vst3/Vst3Editor.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3ModuleLoader.h"

#include <algorithm>
#include <utility>

namespace audient::production
{

namespace
{
constexpr double kSampleRate = 48000.0; // v1 fixed engine sample rate
}

ChannelVstChain::ChannelVstChain(channel::ChannelIdentity identity)
    : m_input(std::move(identity))
{
    m_live.reserve(kMaxSlots);
    m_retiring.reserve(kMaxSlots);
}

ChannelVstChain::~ChannelVstChain()
{
    // Destructor teardown requires the RT callback to be detached (contract in
    // the header). No blocking wait is ever added to the audio thread.
    quiescentShutdown();
}

void ChannelVstChain::configure(std::size_t maxBlockSamples)
{
    m_maxBlock = maxBlockSamples;
    m_chain.configure(maxBlockSamples);
}

const channel::ChannelIdentity& ChannelVstChain::identity() const
{
    return m_input.identity();
}

std::uint32_t ChannelVstChain::latencySamples() const
{
    return m_chain.totalLatencySamples();
}

// --- queries ----------------------------------------------------------------

std::size_t ChannelVstChain::slotCount() const
{
    return m_live.size();
}

bool ChannelVstChain::empty() const
{
    return m_live.empty();
}

bool ChannelVstChain::full() const
{
    return m_live.size() >= kMaxSlots;
}

bool ChannelVstChain::hasSlot(SlotId id) const
{
    return findLive(id) != nullptr;
}

SlotId ChannelVstChain::slotIdAt(std::size_t index) const
{
    if (index >= m_live.size())
    {
        return 0;
    }
    return m_live[index]->id;
}

std::vector<SlotId> ChannelVstChain::slotIds() const
{
    std::vector<SlotId> ids;
    ids.reserve(m_live.size());
    for (const BundlePtr& bundle : m_live)
    {
        ids.push_back(bundle->id);
    }
    return ids;
}

bool ChannelVstChain::slotInfoAt(std::size_t index, SlotInfo& out) const
{
    if (index >= m_live.size())
    {
        return false;
    }
    const SlotBundle& bundle = *m_live[index];
    out.id = bundle.id;
    out.name = bundle.name;
    out.bypass = bundle.bypass;
    return true;
}

bool ChannelVstChain::slotInfoById(SlotId id, SlotInfo& out) const
{
    const SlotBundle* bundle = findLive(id);
    if (bundle == nullptr)
    {
        return false;
    }
    out.id = bundle->id;
    out.name = bundle->name;
    out.bypass = bundle->bypass;
    return true;
}

std::size_t ChannelVstChain::pendingRetirementCount() const
{
    return m_retiring.size();
}

bool ChannelVstChain::hasPendingRetirements() const
{
    return !m_retiring.empty();
}

std::size_t ChannelVstChain::openEditorCount() const
{
    std::size_t count = 0;
    for (const BundlePtr& bundle : m_live)
    {
        if (bundle->editor != nullptr)
        {
            ++count;
        }
    }
    return count;
}

// --- internal helpers ---------------------------------------------------------

ChannelVstChain::SlotBundle* ChannelVstChain::findLive(SlotId id)
{
    for (BundlePtr& bundle : m_live)
    {
        if (bundle->id == id)
        {
            return bundle.get();
        }
    }
    return nullptr;
}

const ChannelVstChain::SlotBundle* ChannelVstChain::findLive(SlotId id) const
{
    for (const BundlePtr& bundle : m_live)
    {
        if (bundle->id == id)
        {
            return bundle.get();
        }
    }
    return nullptr;
}

std::size_t ChannelVstChain::liveIndex(SlotId id) const
{
    for (std::size_t i = 0; i < m_live.size(); ++i)
    {
        if (m_live[i]->id == id)
        {
            return i;
        }
    }
    return m_live.size();
}

void ChannelVstChain::closeEditorOf(SlotBundle& bundle)
{
    if (bundle.editor == nullptr)
    {
        return;
    }
    if (bundle.host != nullptr)
    {
        bundle.host->setParameterEditSink(nullptr);
    }
    bundle.editor->close();
    bundle.editor.reset();
}

bool ChannelVstChain::publishSnapshot(std::vector<audient::vst3::Vst3Chain::Slot> snapshot,
                                        bool wholeChainBypass)
{
    if (!m_chain.publish(std::move(snapshot), wholeChainBypass))
    {
        m_lastError = "chain publish rejected (should not happen at the product cap of 4)";
        return false;
    }
    return true;
}

ChainResult ChannelVstChain::commitPrepared(BundlePtr bundle, std::string name, std::size_t index)
{
    if (bundle == nullptr || bundle->processor == nullptr || !bundle->processor->valid())
    {
        m_lastError = "a valid prepared processor is required";
        return ChainResult::Invalid;
    }
    if (m_shutdown)
    {
        m_lastError = "channel is shut down";
        return ChainResult::Invalid;
    }
    if (m_input.rack().full() || m_live.size() >= kMaxSlots)
    {
        return ChainResult::Full;
    }

    const std::size_t target = std::min(index, m_live.size());
    const SlotId id = m_nextSlotId++;
    bundle->id = id;
    bundle->name = name;

    // Build the complete published order before changing the rack or live list.
    // All later container operations use pre-reserved capacity and noexcept
    // unique_ptr moves.
    std::vector<audient::vst3::Vst3Chain::Slot> snapshot;
    snapshot.reserve(m_live.size() + 1);
    for (std::size_t i = 0; i < m_live.size() + 1; ++i)
    {
        if (i == target)
        {
            snapshot.push_back({bundle->processor.get(), bundle->bypass});
        }
        else
        {
            const std::size_t source = i > target ? i - 1 : i;
            snapshot.push_back({m_live[source]->processor.get(), m_live[source]->bypass});
        }
    }

    channel::ChannelRack& rack = m_input.rack();
    channel::ChannelRack::Slot tag;
    tag.name = name;
    tag.id = id;
    tag.bypass = false;
    tag.missing = false;
    const channel::ChannelRackResult rackResult = rack.add(std::move(tag));
    if (rackResult != channel::ChannelRackResult::Ok)
    {
        m_lastError = "rack commit failed after preparation";
        return ChainResult::Invalid;
    }

    const bool inserting = target < m_live.size();
    if (inserting)
    {
        m_live.insert(m_live.begin() + static_cast<std::ptrdiff_t>(target), std::move(bundle));
        rack.move(rack.size() - 1, target);
    }
    else
    {
        m_live.push_back(std::move(bundle));
    }
    if (!publishSnapshot(std::move(snapshot), m_wholeChainBypass))
    {
        return ChainResult::Invalid;
    }
    return ChainResult::Ok;
}

void ChannelVstChain::releaseAll()
{
    for (BundlePtr& bundle : m_live)
    {
        closeEditorOf(*bundle);
    }
    m_live.clear();
    for (BundlePtr& bundle : m_retiring)
    {
        closeEditorOf(*bundle);
    }
    m_retiring.clear();
}

// --- structural mutation ------------------------------------------------------

ChainResult ChannelVstChain::loadSlot(const std::string& vst3ModulePath)
{
    return loadSlotAt(vst3ModulePath, m_live.size());
}

ChainResult ChannelVstChain::loadSlotAt(const std::string& vst3ModulePath, std::size_t index)
{
    if (m_shutdown)
    {
        m_lastError = "channel is shut down";
        return ChainResult::Invalid;
    }
    if (m_maxBlock == 0)
    {
        m_lastError = "configure(maxBlockSamples) must be called before loading processors";
        return ChainResult::Invalid;
    }
    if (m_live.size() >= kMaxSlots)
    {
        return ChainResult::Full;
    }

    // Prepare fully OFF to the side: only a successful load/prepare commits.
    auto bundle = std::make_unique<SlotBundle>();
    bundle->path = vst3ModulePath;
    bundle->loader = std::make_unique<audient::vst3::Vst3ModuleLoader>();
    if (!bundle->loader->load(vst3ModulePath))
    {
        m_lastError = bundle->loader->lastError();
        return ChainResult::Invalid;
    }
    bundle->host = std::make_unique<audient::vst3::Vst3Host>();
    if (!bundle->host->attachFactory(bundle->loader->factory()))
    {
        m_lastError = bundle->host->lastError();
        return ChainResult::Invalid;
    }
    const std::vector<audient::vst3::PluginClassInfo> classes = bundle->host->classes();
    const audient::vst3::PluginClassInfo* effect = nullptr;
    for (const audient::vst3::PluginClassInfo& cls : classes)
    {
        if (cls.isEffect())
        {
            effect = &cls;
            break;
        }
    }
    if (effect == nullptr)
    {
        m_lastError = "no Audio Effect class in module: " + vst3ModulePath;
        return ChainResult::Invalid;
    }
    bundle->processor = bundle->host->createEffectProcessor(*effect, kSampleRate, static_cast<long>(m_maxBlock),
                                                            audient::vst3::BusLayout::Mono);
    if (bundle->processor == nullptr || !bundle->processor->valid())
    {
        m_lastError = bundle->host->lastError();
        return ChainResult::Invalid;
    }

    const std::string name = effect->name;
    return commitPrepared(std::move(bundle), name, index);
}

ChainResult ChannelVstChain::addPrepared(std::unique_ptr<audient::vst3::Vst3Processor> processor,
                                                          std::string name)
{
    if (m_shutdown)
    {
        m_lastError = "channel is shut down";
        return ChainResult::Invalid;
    }
    if (processor == nullptr || !processor->valid())
    {
        m_lastError = "a valid prepared processor is required";
        return ChainResult::Invalid;
    }
    if (m_live.size() >= kMaxSlots)
    {
        return ChainResult::Full;
    }

    auto bundle = std::make_unique<SlotBundle>();
    bundle->processor = std::move(processor);
    return commitPrepared(std::move(bundle), std::move(name), m_live.size());
}

ChainResult ChannelVstChain::removeSlot(SlotId id)
{
    if (m_shutdown)
    {
        return ChainResult::Invalid;
    }
    const std::size_t index = liveIndex(id);
    if (index >= m_live.size())
    {
        return ChainResult::NotFound;
    }

    std::vector<audient::vst3::Vst3Chain::Slot> snapshot;
    snapshot.reserve(m_live.size() - 1);
    for (std::size_t i = 0; i < m_live.size(); ++i)
    {
        if (i != index)
        {
            snapshot.push_back({m_live[i]->processor.get(), m_live[i]->bypass});
        }
    }

    m_retiring.reserve(m_retiring.size() + 1);
    if (!publishSnapshot(std::move(snapshot), m_wholeChainBypass))
    {
        return ChainResult::Invalid;
    }
    closeEditorOf(*m_live[index]);
    (void)m_input.rack().removeAt(index);
    m_retiring.push_back(std::move(m_live[index]));
    m_live.erase(m_live.begin() + static_cast<std::ptrdiff_t>(index));
    return ChainResult::Ok;
}

ChainResult ChannelVstChain::moveSlot(SlotId id, std::size_t toIndex)
{
    if (m_shutdown)
    {
        return ChainResult::Invalid;
    }
    const std::size_t from = liveIndex(id);
    if (from >= m_live.size())
    {
        return ChainResult::NotFound;
    }
    const std::size_t to = std::min(toIndex, m_live.size() - 1);
    if (to == from)
    {
        return ChainResult::Ok; // no-op reorder; identity unchanged
    }

    std::vector<audient::vst3::Vst3Chain::Slot> snapshot;
    snapshot.reserve(m_live.size());
    for (std::size_t i = 0; i < m_live.size(); ++i)
    {
        std::size_t source = i;
        if (i == to)
        {
            source = from;
        }
        else if (from < to && i >= from && i < to)
        {
            source = i + 1;
        }
        else if (from > to && i > to && i <= from)
        {
            source = i - 1;
        }
        snapshot.push_back({m_live[source]->processor.get(), m_live[source]->bypass});
    }

    if (!publishSnapshot(std::move(snapshot), m_wholeChainBypass))
    {
        return ChainResult::Invalid;
    }

    BundlePtr bundle = std::move(m_live[from]);
    m_live.erase(m_live.begin() + static_cast<std::ptrdiff_t>(from));
    m_live.insert(m_live.begin() + static_cast<std::ptrdiff_t>(to), std::move(bundle));
    (void)m_input.rack().move(from, to);
    return ChainResult::Ok;
}

ChainResult ChannelVstChain::setSlotBypass(SlotId id, bool bypass)
{
    if (m_shutdown)
    {
        return ChainResult::Invalid;
    }
    const std::size_t index = liveIndex(id);
    if (index >= m_live.size())
    {
        return ChainResult::NotFound;
    }
    std::vector<audient::vst3::Vst3Chain::Slot> snapshot;
    snapshot.reserve(m_live.size());
    for (std::size_t i = 0; i < m_live.size(); ++i)
    {
        snapshot.push_back({m_live[i]->processor.get(), i == index ? bypass : m_live[i]->bypass});
    }

    if (!publishSnapshot(std::move(snapshot), m_wholeChainBypass))
    {
        return ChainResult::Invalid;
    }
    m_live[index]->bypass = bypass;
    channel::ChannelRack::Slot* tag = m_input.rack().slot(index);
    if (tag != nullptr)
    {
        tag->bypass = bypass;
    }
    return ChainResult::Ok;
}

void ChannelVstChain::setWholeChainBypass(bool bypass)
{
    if (m_shutdown)
    {
        return;
    }
    std::vector<audient::vst3::Vst3Chain::Slot> snapshot;
    snapshot.reserve(m_live.size());
    for (const BundlePtr& bundle : m_live)
    {
        snapshot.push_back({bundle->processor.get(), bundle->bypass});
    }

    if (!publishSnapshot(std::move(snapshot), bypass))
    {
        m_lastError = "whole-chain bypass publication failed";
        return;
    }
    m_input.rack().setWholeChainBypass(bypass);
    m_wholeChainBypass = bypass;
    m_lastError.clear();

}

bool ChannelVstChain::wholeChainBypass() const
{
    return m_wholeChainBypass;
}

// --- component / controller state ---------------------------------------------

bool ChannelVstChain::saveComponentState(SlotId id, std::vector<std::uint8_t>& out) const
{
    const SlotBundle* bundle = findLive(id);
    if (bundle == nullptr || bundle->processor == nullptr)
    {
        m_lastError = "slot not found or not prepared";
        return false;
    }
    return bundle->processor->saveState(out);
}

bool ChannelVstChain::restoreComponentState(SlotId id, const std::vector<std::uint8_t>& blob)
{
    SlotBundle* bundle = findLive(id);
    if (bundle == nullptr || bundle->processor == nullptr)
    {
        m_lastError = "slot not found or not prepared";
        return false;
    }
    return bundle->processor->restoreState(blob);
}

bool ChannelVstChain::saveControllerState(SlotId id, std::vector<std::uint8_t>& out)
{
    SlotBundle* bundle = findLive(id);
    if (bundle == nullptr || bundle->host == nullptr || bundle->processor == nullptr)
    {
        m_lastError = "controller state requires a loadSlot* slot (loader + host)";
        return false;
    }
    audient::vst3::Vst3Controller controller;
    if (!controller.open(bundle->processor->component(), bundle->loader->factory(), bundle->host->hostContext()))
    {
        m_lastError = controller.lastError();
        return false;
    }
    const bool ok = controller.saveControllerState(out);
    controller.close();
    return ok;
}

bool ChannelVstChain::restoreControllerState(SlotId id, const std::vector<std::uint8_t>& blob)
{
    SlotBundle* bundle = findLive(id);
    if (bundle == nullptr || bundle->host == nullptr || bundle->processor == nullptr)
    {
        m_lastError = "controller state requires a loadSlot* slot (loader + host)";
        return false;
    }
    audient::vst3::Vst3Controller controller;
    if (!controller.open(bundle->processor->component(), bundle->loader->factory(), bundle->host->hostContext()))
    {
        m_lastError = controller.lastError();
        return false;
    }
    const bool ok = controller.restoreControllerState(blob);
    controller.close();
    return ok;
}

// --- editor ownership (per stable SlotId) -------------------------------------

ChainResult ChannelVstChain::openEditor(SlotId id, void* parentWindow)
{
    if (m_shutdown)
    {
        return ChainResult::Invalid;
    }
    SlotBundle* bundle = findLive(id);
    if (bundle == nullptr)
    {
        return ChainResult::NotFound;
    }
    if (bundle->host == nullptr || bundle->loader == nullptr || bundle->processor == nullptr)
    {
        m_lastError = "editors require a loadSlot* slot (loader + host)";
        return ChainResult::Invalid;
    }
    if (bundle->editor != nullptr)
    {
        closeEditor(id); // one editor per slot: a new open replaces the old
    }
    bundle->editor = std::make_unique<audient::vst3::Vst3Editor>();
    if (!bundle->editor->open(bundle->processor->component(), bundle->loader->factory(), parentWindow,
                              bundle->host->hostContext()))
    {
        m_lastError = bundle->editor->lastError();
        bundle->editor.reset();
        return ChainResult::Invalid;
    }
    bundle->host->setParameterEditSink(bundle->processor.get());
    m_lastError.clear();
    return ChainResult::Ok;
}

ChainResult ChannelVstChain::closeEditor(SlotId id)
{
    if (m_shutdown)
    {
        return ChainResult::Invalid;
    }
    SlotBundle* bundle = findLive(id);
    if (bundle == nullptr)
    {
        return ChainResult::NotFound;
    }
    closeEditorOf(*bundle);
    return ChainResult::Ok;
}

bool ChannelVstChain::isEditorOpen(SlotId id) const
{
    const SlotBundle* bundle = findLive(id);
    return bundle != nullptr && bundle->editor != nullptr;
}

// --- retirement ----------------------------------------------------------------

void ChannelVstChain::serviceRetirements()
{
    if (m_shutdown)
    {
        return;
    }
    // Reaps expired immutable snapshots; only then are bundles whose processors
    // can no longer be referenced by any in-flight/retired snapshot destroyed.
    m_chain.reap();
    if (!m_chain.settled())
    {
        return;
    }
    for (BundlePtr& bundle : m_retiring)
    {
        closeEditorOf(*bundle);
    }
    m_retiring.clear();
}

// --- quiescent shutdown ---------------------------------------------------------

void ChannelVstChain::quiescentShutdown()
{
    if (m_shutdown)
    {
        return;
    }
    m_shutdown = true;
    m_chain.quiescentShutdown();
    releaseAll();
    m_input.rack().clear();
    m_wholeChainBypass = false;
}

} // namespace audient::production
