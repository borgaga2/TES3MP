#include "activespells.hpp"

#include <algorithm>
#include <optional>

#include <components/debug/debuglog.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/stringops.hpp>
#include <components/misc/resourcehelpers.hpp>

#include <components/esm3/loadmgef.hpp>

#include <components/settings/settings.hpp>

#include "creaturestats.hpp"
#include "spellcasting.hpp"
#include "spelleffects.hpp"

/*
    Start of tes3mp addition

    Include additional headers for multiplayer purposes
*/
#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwworld/class.hpp"
#include "../mwmp/Main.hpp"
#include "../mwmp/LocalPlayer.hpp"
#include "../mwmp/CellController.hpp"
#include "../mwmp/MechanicsHelper.hpp"
/*
    End of tes3mp addition
*/

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/animation.hpp"

#include "../mwworld/esmstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/inventorystore.hpp"

namespace
{
    bool merge(std::vector<ESM::ActiveEffect>& present, const std::vector<ESM::ActiveEffect>& queued)
    {
        // Can't merge if we already have an effect with the same effect index
        auto problem = std::find_if(queued.begin(), queued.end(), [&] (const auto& qEffect)
        {
            return std::find_if(present.begin(), present.end(), [&] (const auto& pEffect) { return pEffect.mEffectIndex == qEffect.mEffectIndex; }) != present.end();
        });
        if(problem != queued.end())
            return false;
        present.insert(present.end(), queued.begin(), queued.end());
        return true;
    }

    void addEffects(std::vector<ESM::ActiveEffect>& effects, const ESM::EffectList& list, bool ignoreResistances = false)
    {
        int currentEffectIndex = 0;
        for(const auto& enam : list.mList)
        {
            ESM::ActiveEffect effect;
            effect.mEffectId = enam.mEffectID;
            effect.mArg = MWMechanics::EffectKey(enam).mArg;
            effect.mMagnitude = 0.f;
            effect.mMinMagnitude = enam.mMagnMin;
            effect.mMaxMagnitude = enam.mMagnMax;
            effect.mEffectIndex = currentEffectIndex++;
            effect.mFlags = ESM::ActiveEffect::Flag_None;
            if(ignoreResistances)
                effect.mFlags |= ESM::ActiveEffect::Flag_Ignore_Resistances;
            effect.mDuration = -1;
            effect.mTimeLeft = -1;
            effects.emplace_back(effect);
        }
    }
}

namespace MWMechanics
{
    ActiveSpells::IterationGuard::IterationGuard(ActiveSpells& spells) : mActiveSpells(spells)
    {
        mActiveSpells.mIterating = true;
    }

    ActiveSpells::IterationGuard::~IterationGuard()
    {
        mActiveSpells.mIterating = false;
    }

    ActiveSpells::ActiveSpellParams::ActiveSpellParams(const CastSpell& cast, const MWWorld::Ptr& caster)
    : mId(cast.mId), mDisplayName(cast.mSourceName), mCasterActorId(-1), mSlot(cast.mSlot), mType(cast.mType), mWorsenings(-1)
    {
        if(!caster.isEmpty() && caster.getClass().isActor())
            mCasterActorId = caster.getClass().getCreatureStats(caster).getActorId();
    }

    ActiveSpells::ActiveSpellParams::ActiveSpellParams(const ESM::Spell* spell, const MWWorld::Ptr& actor, bool ignoreResistances)
    : mId(spell->mId), mDisplayName(spell->mName), mCasterActorId(actor.getClass().getCreatureStats(actor).getActorId()), mSlot(0)
    , mType(spell->mData.mType == ESM::Spell::ST_Ability ? ESM::ActiveSpells::Type_Ability : ESM::ActiveSpells::Type_Permanent), mWorsenings(-1)
    {
        assert(spell->mData.mType != ESM::Spell::ST_Spell && spell->mData.mType != ESM::Spell::ST_Power);
        addEffects(mEffects, spell->mEffects, ignoreResistances);
    }

    ActiveSpells::ActiveSpellParams::ActiveSpellParams(const MWWorld::ConstPtr& item, const ESM::Enchantment* enchantment, int slotIndex, const MWWorld::Ptr& actor)
    : mId(item.getCellRef().getRefId()), mDisplayName(item.getClass().getName(item)), mCasterActorId(actor.getClass().getCreatureStats(actor).getActorId())
    , mSlot(slotIndex), mType(ESM::ActiveSpells::Type_Enchantment), mWorsenings(-1)
    {
        assert(enchantment->mData.mType == ESM::Enchantment::ConstantEffect);
        addEffects(mEffects, enchantment->mEffects);
    }

    ActiveSpells::ActiveSpellParams::ActiveSpellParams(const ESM::ActiveSpells::ActiveSpellParams& params)
    : mId(params.mId), mEffects(params.mEffects), mDisplayName(params.mDisplayName), mCasterActorId(params.mCasterActorId)
    , mSlot(params.mItem.isSet() ? params.mItem.mIndex : 0)
    , mType(params.mType), mWorsenings(params.mWorsenings), mNextWorsening({params.mNextWorsening})
    {}

    ActiveSpells::ActiveSpellParams::ActiveSpellParams(const ActiveSpellParams& params, const MWWorld::Ptr& actor)
    : mId(params.mId), mDisplayName(params.mDisplayName), mCasterActorId(actor.getClass().getCreatureStats(actor).getActorId())
    , mSlot(params.mSlot), mType(params.mType), mWorsenings(-1), mTimeStamp(params.mTimeStamp)
    {}

    ESM::ActiveSpells::ActiveSpellParams ActiveSpells::ActiveSpellParams::toEsm() const
    {
        ESM::ActiveSpells::ActiveSpellParams params;
        params.mId = mId;
        params.mEffects = mEffects;
        params.mDisplayName = mDisplayName;
        params.mCasterActorId = mCasterActorId;
        params.mItem.unset();
        if(mSlot)
        {
            // Note that we're storing the inventory slot as a RefNum instead of an int as a matter of future proofing
            // mSlot needs to be replaced with a RefNum once inventory items get persistent RefNum (#4508 #6148)
            params.mItem = { static_cast<unsigned int>(mSlot), 0 };
        }
        params.mType = mType;
        params.mWorsenings = mWorsenings;
        params.mNextWorsening = mNextWorsening.toEsm();
        return params;
    }

    void ActiveSpells::ActiveSpellParams::worsen()
    {
        ++mWorsenings;
        if(!mWorsenings)
            mNextWorsening = MWBase::Environment::get().getWorld()->getTimeStamp();
        mNextWorsening += CorprusStats::sWorseningPeriod;
    }

    bool ActiveSpells::ActiveSpellParams::shouldWorsen() const
    {
        return mWorsenings >= 0 && MWBase::Environment::get().getWorld()->getTimeStamp() >= mNextWorsening;
    }

    void ActiveSpells::ActiveSpellParams::resetWorsenings()
    {
        mWorsenings = -1;
    }

    void ActiveSpells::update(const MWWorld::Ptr& ptr, float duration)
    {
        if (mIterating)
            return;
        auto& creatureStats = ptr.getClass().getCreatureStats(ptr);
        assert(&creatureStats.getActiveSpells() == this);
        IterationGuard guard{*this};
        // Erase no longer active spells and effects
        for(auto spellIt = mSpells.begin(); spellIt != mSpells.end();)
        {
            if(spellIt->mType != ESM::ActiveSpells::Type_Temporary && spellIt->mType != ESM::ActiveSpells::Type_Consumable)
            {
                ++spellIt;
                continue;
            }
            bool removedSpell = false;
            for(auto effectIt = spellIt->mEffects.begin(); effectIt != spellIt->mEffects.end();)
            {
                if(effectIt->mFlags & ESM::ActiveEffect::Flag_Remove && effectIt->mTimeLeft <= 0.f)
                {
                    auto effect = *effectIt;
                    effectIt = spellIt->mEffects.erase(effectIt);
                    onMagicEffectRemoved(ptr, *spellIt, effect);
                    removedSpell = applyPurges(ptr, &spellIt, &effectIt);
                    if(removedSpell)
                        break;
                }
                else
                {
                    ++effectIt;
                }
            }
            if(removedSpell)
                continue;
            if(spellIt->mEffects.empty())
            {
                /*
                    Start of tes3mp addition

                    Whenever the local player loses an active spell, send an ID_PLAYER_SPELLS_ACTIVE packet to the server with it

                    Whenever a local actor loses an active spell, send an ID_ACTOR_SPELLS_ACTIVE packet to the server with it
                */
                if (this == &MWMechanics::getPlayer().getClass().getCreatureStats(MWMechanics::getPlayer()).getActiveSpells())
                {
                    mwmp::Main::get().getLocalPlayer()->sendSpellsActiveRemoval(spellIt->mId,
                        MechanicsHelper::isStackingSpell(spellIt->mId), spellIt->mTimeStamp);
                }
                else
                {
                    MWWorld::Ptr actorPtr = MWBase::Environment::get().getWorld()->searchPtrViaActorId(getActorId());

                    if (mwmp::Main::get().getCellController()->isLocalActor(actorPtr))
                        mwmp::Main::get().getCellController()->getLocalActor(actorPtr)->sendSpellsActiveRemoval(spellIt->mId,
                            MechanicsHelper::isStackingSpell(spellIt->mId), spellIt->mTimeStamp);
                }
                /*
                    End of tes3mp addition
                */

                spellIt = mSpells.erase(spellIt);
            }
            else
                ++spellIt;
        }

        for(const auto& spell : mQueue)
            addToSpells(ptr, spell);
        mQueue.clear();

        // Vanilla only does this on cell change I think
        const auto& spells = creatureStats.getSpells();
        for(const ESM::Spell* spell : spells)
        {
            if(spell->mData.mType != ESM::Spell::ST_Spell && spell->mData.mType != ESM::Spell::ST_Power && !isSpellActive(spell->mId))
                mSpells.emplace_back(ActiveSpellParams{spell, ptr});
        }

        if(ptr.getClass().hasInventoryStore(ptr) && !(creatureStats.isDead() && !creatureStats.isDeathAnimationFinished()))
        {
            auto& store = ptr.getClass().getInventoryStore(ptr);
            if(store.getInvListener() != nullptr)
            {
                bool playNonLooping = !store.isFirstEquip();
                const auto world = MWBase::Environment::get().getWorld();
                for(int slotIndex = 0; slotIndex < MWWorld::InventoryStore::Slots; slotIndex++)
                {
                    auto slot = store.getSlot(slotIndex);
                    if(slot == store.end())
                        continue;
                    const auto& enchantmentId = slot->getClass().getEnchantment(*slot);
                    if(enchantmentId.empty())
                        continue;
                    const ESM::Enchantment* enchantment = world->getStore().get<ESM::Enchantment>().find(enchantmentId);
                    if(enchantment->mData.mType != ESM::Enchantment::ConstantEffect)
                        continue;
                    if(std::find_if(mSpells.begin(), mSpells.end(), [&] (const ActiveSpellParams& params)
                    {
                        return params.mSlot == slotIndex && params.mType == ESM::ActiveSpells::Type_Enchantment && params.mId == slot->getCellRef().getRefId();
                    }) != mSpells.end())
                        continue;
                    // world->breakInvisibility leads to a stack overflow as it calls this method so just break invisibility manually
                    purgeEffect(ptr, ESM::MagicEffect::Invisibility);
                    applyPurges(ptr);
                    const ActiveSpellParams& params = mSpells.emplace_back(ActiveSpellParams{*slot, enchantment, slotIndex, ptr});
                    for(const auto& effect : params.mEffects)
                        MWMechanics::playEffects(ptr, *world->getStore().get<ESM::MagicEffect>().find(effect.mEffectId), playNonLooping);
                }
            }
        }

        // Update effects
        for(auto spellIt = mSpells.begin(); spellIt != mSpells.end();)
        {
            const auto caster = MWBase::Environment::get().getWorld()->searchPtrViaActorId(spellIt->mCasterActorId); //Maybe make this search outside active grid?
            bool removedSpell = false;
            std::optional<ActiveSpellParams> reflected;
            for(auto it = spellIt->mEffects.begin(); it != spellIt->mEffects.end();)
            {
                auto result = applyMagicEffect(ptr, caster, *spellIt, *it, duration);
                if(result == MagicApplicationResult::REFLECTED)
                {
                    if(!reflected)
                    {
                        static const bool keepOriginalCaster = Settings::Manager::getBool("classic reflected absorb spells behavior", "Game");
                        if(keepOriginalCaster)
                            reflected = {*spellIt, caster};
                        else
                            reflected = {*spellIt, ptr};
                    }
                    auto& reflectedEffect = reflected->mEffects.emplace_back(*it);
                    reflectedEffect.mFlags = ESM::ActiveEffect::Flag_Ignore_Reflect | ESM::ActiveEffect::Flag_Ignore_SpellAbsorption;
                    it = spellIt->mEffects.erase(it);
                }
                else if(result == MagicApplicationResult::REMOVED)
                    it = spellIt->mEffects.erase(it);
                else
                    ++it;
                removedSpell = applyPurges(ptr, &spellIt, &it);
                if(removedSpell)
                    break;
            }
            if(reflected)
            {
                const ESM::Static* reflectStatic = MWBase::Environment::get().getWorld()->getStore().get<ESM::Static>().find("VFX_Reflect");
                MWRender::Animation* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr);
                if(animation && !reflectStatic->mModel.empty())
                {
                    const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
                    animation->addEffect(
                        Misc::ResourceHelpers::correctMeshPath(reflectStatic->mModel, vfs),
                        ESM::MagicEffect::Reflect, false, std::string());
                }
                caster.getClass().getCreatureStats(caster).getActiveSpells().addSpell(*reflected);
            }
            if(removedSpell)
                continue;

            bool remove = false;
            if(spellIt->mType == ESM::ActiveSpells::Type_Ability || spellIt->mType == ESM::ActiveSpells::Type_Permanent)
            {
                try
                {
                    remove = !spells.hasSpell(spellIt->mId);
                }
                catch(const std::runtime_error& e)
                {
                    remove = true;
                    Log(Debug::Error) << "Removing active effect: " << e.what();
                }
            }
            else if(spellIt->mType == ESM::ActiveSpells::Type_Enchantment)
            {
                const auto& store = ptr.getClass().getInventoryStore(ptr);
                auto slot = store.getSlot(spellIt->mSlot);
                remove = slot == store.end() || slot->getCellRef().getRefId() != spellIt->mId;
            }
            if(remove)
            {
                auto params = *spellIt;
                spellIt = mSpells.erase(spellIt);
                for(const auto& effect : params.mEffects)
                    onMagicEffectRemoved(ptr, params, effect);
                applyPurges(ptr, &spellIt);
                continue;
            }
            ++spellIt;
        }

        static const bool keepCalm = Settings::Manager::getBool("classic calm spells behavior", "Game");
        if (keepCalm)
        {
            ESM::MagicEffect::Effects effect
                = ptr.getClass().isNpc() ? ESM::MagicEffect::CalmHumanoid : ESM::MagicEffect::CalmCreature;
            if (creatureStats.getMagicEffects().get(effect).getMagnitude() > 0.f)
                creatureStats.getAiSequence().stopCombat();
        }
    }

    void ActiveSpells::addToSpells(const MWWorld::Ptr& ptr, const ActiveSpellParams& spell)
    {
        if(spell.mType != ESM::ActiveSpells::Type_Consumable)
        {
            auto found = std::find_if(mSpells.begin(), mSpells.end(), [&] (const auto& existing)
            {
                return spell.mId == existing.mId && spell.mCasterActorId == existing.mCasterActorId && spell.mSlot == existing.mSlot;
            });
            if(found != mSpells.end())
            {
                if(merge(found->mEffects, spell.mEffects))
                    return;
                auto params = *found;
                mSpells.erase(found);
                for(const auto& effect : params.mEffects)
                    onMagicEffectRemoved(ptr, params, effect);
            }
        }
        mSpells.emplace_back(spell);
    }

    ActiveSpells::ActiveSpells() : mIterating(false)
    /*
        Start of tes3mp addition

        Initialize the actorId as invalid
    */
    , mActorId(-1)
    /*
        End of tes3mp addition
    */
    {}

    ActiveSpells::TIterator ActiveSpells::begin() const
    {
        return mSpells.begin();
    }

    ActiveSpells::TIterator ActiveSpells::end() const
    {
        return mSpells.end();
    }

    bool ActiveSpells::isSpellActive(std::string_view id) const
    {
        return std::find_if(mSpells.begin(), mSpells.end(), [&] (const auto& spell)
        {
            return Misc::StringUtils::ciEqual(spell.mId, id);
        }) != mSpells.end();
    }

    void ActiveSpells::addSpell(const ActiveSpellParams& params)
    {
        /*
            Start of tes3mp change (major)

            Give the spell a timestamp and, whenever a player or local actor gains an active spell as a result
            of gameplay, send an ID_PLAYER_SPELLS_ACTIVE or ID_ACTOR_SPELLS_ACTIVE packet to the server with it
        */
        ActiveSpellParams timestampedParams = params;
        timestampedParams.mTimeStamp = MWBase::Environment::get().getWorld()->getTimeStamp();

        mQueue.emplace_back(timestampedParams);

        const bool isStacking = MechanicsHelper::isStackingSpell(timestampedParams.mId);

        if (this == &MWMechanics::getPlayer().getClass().getCreatureStats(MWMechanics::getPlayer()).getActiveSpells())
        {
            mwmp::Main::get().getLocalPlayer()->sendSpellsActiveAddition(timestampedParams.mId, isStacking, timestampedParams);
        }
        else
        {
            MWWorld::Ptr actorPtr = MWBase::Environment::get().getWorld()->searchPtrViaActorId(getActorId());

            if (mwmp::Main::get().getCellController()->isLocalActor(actorPtr))
                mwmp::Main::get().getCellController()->getLocalActor(actorPtr)->sendSpellsActiveAddition(timestampedParams.mId, isStacking, timestampedParams);
        }
        /*
            End of tes3mp change (major)
        */
    }

    void ActiveSpells::addSpell(const ESM::Spell* spell, const MWWorld::Ptr& actor)
    {
        mQueue.emplace_back(ActiveSpellParams{spell, actor, true});
    }

    /*
        Start of tes3mp change (major)

        Add a timestamp argument so spells received from other clients can have the same timestamps they had there,
        as well as a sendPacket argument used to prevent packets from being sent back to the server when we've just
        received them from it
    */
    void ActiveSpells::addSpell(const std::string& id, bool stack, std::vector<ActiveEffect> effects,
                                const std::string& displayName, int casterActorId, MWWorld::TimeStamp timestamp, bool sendPacket)
    {
        ESM::ActiveSpells::ActiveSpellParams esmParams;
        esmParams.mId = id;
        esmParams.mEffects = effects;
        esmParams.mDisplayName = displayName;
        esmParams.mCasterActorId = casterActorId;
        esmParams.mItem.unset();
        esmParams.mType = stack ? ESM::ActiveSpells::Type_Consumable : ESM::ActiveSpells::Type_Temporary;
        esmParams.mWorsenings = -1;

        ActiveSpellParams params{esmParams};

        /*
            Start of tes3mp addition

            Track the timestamp of this active spell so that, if spells are stacked, the correct one can be removed
        */
        params.mTimeStamp = timestamp;
        /*
            End of tes3mp addition
        */

        mQueue.emplace_back(params);

        /*
            Start of tes3mp addition

            Whenever a player gains an active spell as a result of gameplay, send an ID_PLAYER_SPELLS_ACTIVE packet
            to the server with it
        */
        if (sendPacket)
        {
            if (this == &MWMechanics::getPlayer().getClass().getCreatureStats(MWMechanics::getPlayer()).getActiveSpells())
            {
                mwmp::Main::get().getLocalPlayer()->sendSpellsActiveAddition(id, stack, params);
            }
            else
            {
                MWWorld::Ptr actorPtr = MWBase::Environment::get().getWorld()->searchPtrViaActorId(getActorId());

                if (mwmp::Main::get().getCellController()->isLocalActor(actorPtr))
                    mwmp::Main::get().getCellController()->getLocalActor(actorPtr)->sendSpellsActiveAddition(id, stack, params);
            }
        }
        /*
            End of tes3mp addition
        */
    }
    /*
        End of tes3mp change (major)
    */

    void ActiveSpells::purge(ParamsPredicate predicate, const MWWorld::Ptr& ptr)
    {
        assert(&ptr.getClass().getCreatureStats(ptr).getActiveSpells() == this);
        mPurges.emplace(predicate);
        if(!mIterating)
        {
            IterationGuard guard{*this};
            applyPurges(ptr);
        }
    }

    void ActiveSpells::purge(EffectPredicate predicate, const MWWorld::Ptr& ptr)
    {
        assert(&ptr.getClass().getCreatureStats(ptr).getActiveSpells() == this);
        mPurges.emplace(predicate);
        if(!mIterating)
        {
            IterationGuard guard{*this};
            applyPurges(ptr);
        }
    }

    bool ActiveSpells::applyPurges(const MWWorld::Ptr& ptr, std::list<ActiveSpellParams>::iterator* currentSpell, std::vector<ActiveEffect>::iterator* currentEffect)
    {
        bool removedCurrentSpell = false;
        while(!mPurges.empty())
        {
            auto predicate = mPurges.front();
            mPurges.pop();
            for(auto spellIt = mSpells.begin(); spellIt != mSpells.end();)
            {
                bool isCurrentSpell = currentSpell && *currentSpell == spellIt;
                std::visit([&] (auto&& variant)
                {
                    using T = std::decay_t<decltype(variant)>;
                    if constexpr (std::is_same_v<T, ParamsPredicate>)
                    {
                        if(variant(*spellIt))
                        {
                            auto params = *spellIt;
                            spellIt = mSpells.erase(spellIt);
                            if(isCurrentSpell)
                            {
                                *currentSpell = spellIt;
                                removedCurrentSpell = true;
                            }
                            for(const auto& effect : params.mEffects)
                                onMagicEffectRemoved(ptr, params, effect);
                        }
                        else
                            ++spellIt;
                    }
                    else
                    {
                        static_assert(std::is_same_v<T, EffectPredicate>, "Non-exhaustive visitor");
                        for(auto effectIt = spellIt->mEffects.begin(); effectIt != spellIt->mEffects.end();)
                        {
                            if(variant(*spellIt, *effectIt))
                            {
                                auto effect = *effectIt;
                                if(isCurrentSpell && currentEffect)
                                {
                                    auto distance = std::distance(spellIt->mEffects.begin(), *currentEffect);
                                    if(effectIt <= *currentEffect)
                                        distance--;
                                    effectIt = spellIt->mEffects.erase(effectIt);
                                    *currentEffect = spellIt->mEffects.begin() + distance;
                                }
                                else
                                    effectIt = spellIt->mEffects.erase(effectIt);
                                onMagicEffectRemoved(ptr, *spellIt, effect);
                            }
                            else
                                ++effectIt;
                        }
                        ++spellIt;
                    }
                }, predicate);
            }
        }
        return removedCurrentSpell;
    }

    void ActiveSpells::removeEffects(const MWWorld::Ptr& ptr, std::string_view id)
    {
        purge([=] (const ActiveSpellParams& params)
        {
            return params.mId == id;
        }, ptr);
    }

    /*
        Start of tes3mp addition

        Remove the spell with a certain ID and a certain timestamp, useful
        when there are stacked spells with the same ID

        Returns a boolean that indicates whether the corresponding spell was found
    */
    bool ActiveSpells::removeSpellByTimestamp(const std::string& id, MWWorld::TimeStamp timestamp)
    {
        auto predicate = [id, timestamp] (const ActiveSpellParams& params)
        {
            return params.mId == id && params.mTimeStamp == timestamp;
        };

        bool found = std::find_if(mSpells.begin(), mSpells.end(), predicate) != mSpells.end();

        if (found)
        {
            MWWorld::Ptr ptr = MWBase::Environment::get().getWorld()->searchPtrViaActorId(getActorId());

            if (!ptr.isEmpty())
                purge(predicate, ptr);
            else
                mSpells.remove_if(predicate);
        }

        return found;
    }
    /*
        End of tes3mp addition
    */

    void ActiveSpells::purgeEffect(const MWWorld::Ptr& ptr, short effectId)
    {
        purge([=] (const ActiveSpellParams&, const ESM::ActiveEffect& effect)
        {
            return effect.mEffectId == effectId;
        }, ptr);
    }

    void ActiveSpells::purge(const MWWorld::Ptr& ptr, int casterActorId)
    {
        purge([=] (const ActiveSpellParams& params)
        {
            return params.mCasterActorId == casterActorId;
        }, ptr);
    }

    void ActiveSpells::clear(const MWWorld::Ptr& ptr)
    {
        mQueue.clear();
        purge([] (const ActiveSpellParams& params) { return true; }, ptr);
    }

    void ActiveSpells::skipWorsenings(double hours)
    {
        for(auto& spell : mSpells)
        {
            if(spell.mWorsenings >= 0)
                spell.mNextWorsening += hours;
        }
    }

    /*
        Start of tes3mp addition

        Allow the purging of an effect for a specific arg (attribute or skill)
    */
    void ActiveSpells::purgeEffectByArg(short effectId, int effectArg)
    {
        auto predicate = [effectId, effectArg] (const ActiveSpellParams&, const ESM::ActiveEffect& effect)
        {
            return effect.mEffectId == effectId && effect.mArg == effectArg;
        };

        MWWorld::Ptr ptr = MWBase::Environment::get().getWorld()->searchPtrViaActorId(getActorId());

        if (!ptr.isEmpty())
            purge(predicate, ptr);
        else
        {
            for (auto& spell : mSpells)
            {
                spell.mEffects.erase(std::remove_if(spell.mEffects.begin(), spell.mEffects.end(),
                    [&] (const ESM::ActiveEffect& effect) { return predicate(spell, effect); }), spell.mEffects.end());
            }
        }
    }
    /*
        End of tes3mp addition
    */

    /*
        Start of tes3mp addition

        Make it easy to get an effect's duration
    */
    float ActiveSpells::getEffectDuration(short effectId, std::string sourceId)
    {
        for (const auto& spell : mSpells)
        {
            if (sourceId.compare(spell.mId) == 0)
            {
                for (const auto& effect : spell.mEffects)
                {
                    if (effect.mEffectId == effectId)
                        return effect.mDuration;
                }
            }
        }
        return 0.f;
    }
    /*
        End of tes3mp addition
    */

    void ActiveSpells::writeState(ESM::ActiveSpells &state) const
    {
        for(const auto& spell : mSpells)
            state.mSpells.emplace_back(spell.toEsm());
        for(const auto& spell : mQueue)
            state.mQueue.emplace_back(spell.toEsm());
    }

    void ActiveSpells::readState(const ESM::ActiveSpells &state)
    {
        for(const ESM::ActiveSpells::ActiveSpellParams& spell : state.mSpells)
            mSpells.emplace_back(ActiveSpellParams{spell});
        for(const ESM::ActiveSpells::ActiveSpellParams& spell : state.mQueue)
            mQueue.emplace_back(ActiveSpellParams{spell});
    }

    void ActiveSpells::unloadActor(const MWWorld::Ptr& ptr)
    {
        purge([] (const auto& spell)
        {
            return spell.getType() == ESM::ActiveSpells::Type_Consumable || spell.getType() == ESM::ActiveSpells::Type_Temporary;
        }, ptr);
        mQueue.clear();
    }

    /*
        Start of tes3mp addition

        Make it possible to set and get the actorId for these ActiveSpells
    */
    int ActiveSpells::getActorId() const
    {
        return mActorId;
    }

    void ActiveSpells::setActorId(int actorId)
    {
        mActorId = actorId;
    }
    /*
        End of tes3mp addition
    */
}
