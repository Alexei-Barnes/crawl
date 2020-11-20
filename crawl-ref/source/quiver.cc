/**
 * @file
 * @brief Player quiver functionality
**/

#include "AppHdr.h"

#include "quiver.h"

#include <algorithm>

#include "ability.h"
#include "artefact.h"
#include "art-enum.h"
#include "env.h"
#include "evoke.h"
#include "invent.h"
#include "item-prop.h"
#include "item-use.h"
#include "items.h"
#include "macro.h"
#include "message.h"
#include "options.h"
#include "player.h"
#include "prompt.h"
#include "sound.h"
#include "spl-damage.h"
#include "stringutil.h"
#include "tags.h"
#include "target.h"
#include "throw.h"

static int _get_pack_slot(const item_def&);
static bool _item_matches(const item_def &item, fire_type types,
                          const item_def* launcher, bool manual);
static bool _items_similar(const item_def& a, const item_def& b,
                           bool force = true);

// TODO: how should newquivers integrate with the local tiles UI?

namespace quiver
{
    // Returns the type of ammo used by the player's equipped weapon,
    // or AMMO_THROW if it's not a launcher.
    static launcher _get_weapon_ammo_type(const item_def* weapon)
    {
        if (weapon == nullptr)
            return AMMO_THROW;
        if (weapon->base_type != OBJ_WEAPONS)
            return AMMO_THROW;

        switch (weapon->sub_type)
        {
            case WPN_HUNTING_SLING:
            case WPN_FUSTIBALUS:
                return AMMO_SLING;
            case WPN_SHORTBOW:
            case WPN_LONGBOW:
                return AMMO_BOW;
            case WPN_HAND_CROSSBOW:
            case WPN_ARBALEST:
            case WPN_TRIPLE_CROSSBOW:
                return AMMO_CROSSBOW;
            default:
                return AMMO_THROW;
        }
    }

    void action::reset()
    {
        target = dist();
    };

    /**
     * Does this action meet preconditions for triggering? Checks configurable
     * HP and MP thresholds, aimed at autofight commands.
     * @return true if triggering should be prevented
     */
    bool action::autofight_check() const
    {
        // don't do these checks if the action will lead to interactive targeting
        if (target.needs_targeting())
            return false;
        bool af_hp_check = false;
        bool af_mp_check = false;
        if (!clua.callfn("af_hp_is_low", ">b", &af_hp_check)
            || uses_mp() && !clua.callfn("af_mp_is_low", ">b", &af_mp_check))
        {
            if (!clua.error.empty())
                mprf(MSGCH_ERROR, "Lua error: %s", clua.error.c_str());
            return true;
        }
        if (af_hp_check)
            mpr("You are too injured to fight recklessly!");
        else if (af_mp_check)
            mpr("You are too depleted to draw on your mana recklessly!");
        return af_hp_check || af_mp_check;
    }

    formatted_string action::quiver_description(bool short_desc) const
    {
        return formatted_string::parse_string(
                        short_desc ? "<darkgrey>Empty</darkgrey>"
                                   : "<darkgrey>Nothing quivered</darkgrey>");
    }

    shared_ptr<action> action::find_next(int dir, bool allow_disabled, bool loop) const
    {
        auto o = get_fire_order(allow_disabled);
        if (o.size() == 0)
            return nullptr; // or same type?

        if (dir < 0)
            reverse(o.begin(), o.end());

        if (!is_valid())
            return o[0];

        int i = 0;
        for (; i < static_cast<int>(o.size()); i++)
            if (*o[i] == *this)
                break;

        // the current action is not in the fire order at all; perhaps it is
        // disabled, or skipped for action-specific reasons. Just start at
        // the beginning.
        if (i == static_cast<int>(o.size()))
            return o[0];

        i++;
        if (!loop && i >= static_cast<int>(o.size()))
            return nullptr;

        return o[i % o.size()];
    }

    shared_ptr<action> action_cycler::do_target()
    {
        // this would be better as an action method, but it's tricky without
        // moving untargeted_fire somewhere else
        // should this reset()?

        shared_ptr<action> a = get_ptr();
        if (!a || !a->is_valid())
            return nullptr;

        a->reset();
        a->target.target = coord_def(-1,-1);
        a->target.find_target = false;
        a->target.fire_context = this;
        a->target.interactive = true;

        if (a->is_targeted())
            a->trigger(a->target);
        else
        {
            untargeted_fire(a);
            if (!a->target.isCancel)
                a->trigger();
        }
        // TODO: does this cause dbl "ok then"s in some places?
        if (a->target.isCancel && a->target.cmd_result == CMD_NO_CMD)
            canned_msg(MSG_OK);

        // we return a; if it has become invalid (e.g. by running out of ammo),
        // it will no longer be accessible via get().
        return a;
    }

    string action_cycler::fire_key_hints()
    {
        const bool no_other_items = get() == *next();
        string key_hint = no_other_items
                            ? ", <w>%</w> - select action"
                            : ", <w>%</w> - select action, <w>%</w>/<w>%</w> - cycle";
        insert_commands(key_hint,
                        { CMD_TARGET_SELECT_ACTION,
                          CMD_TARGET_CYCLE_QUIVER_BACKWARD,
                          CMD_TARGET_CYCLE_QUIVER_FORWARD });
        return key_hint;
    }

    static bool _autoswitch_active()
    {
        return Options.auto_switch
                && (you.equip[EQ_WEAPON] == letter_to_index('a')
                    || you.equip[EQ_WEAPON] == letter_to_index('b'));
    }

    static bool _autoswitch_ammo_check(const item_def &ammo)
    {
        if (!ammo.defined())
            return false;
        const item_def &w1 = you.inv[letter_to_index('a')];
        const item_def &w2 = you.inv[letter_to_index('b')];
        return w1.defined() && _item_matches(ammo, (fire_type) 0xffff, &w1, false)
            || w2.defined() && _item_matches(ammo, (fire_type) 0xffff, &w2, false);
    }


    static bool _autoswitch_to_ranged(item_def &ammo)
    {
        // TODO: switching away from ranged weapons with autoswitch on is a bit
        // wonky
        if (!_autoswitch_active())
            return false;

        // validated above
        const int item_slot = you.equip[EQ_WEAPON] == letter_to_index('a')
                                ? letter_to_index('b') : letter_to_index('a');

        const item_def& launcher = you.inv[item_slot];
        if (!_autoswitch_ammo_check(ammo))
            return false;
        if (!ammo.launched_by(launcher))
            return false;

        if (!wield_weapon(true, item_slot))
            return false;

        you.turn_is_over = true;
        // This just does the wield. The old implementation worked by
        // additionally firing immediately, but it seems better to do it step
        // by step to me. Will players dislike this?
        return true;
    }

    // Get a sorted list of items to show in the fire interface.
    //
    // If ignore_inscription_etc, ignore =f and Options.fire_items_start.
    // This is used for generating informational error messages, when the
    // fire order is empty.
    //
    // launcher determines what items match the 'launcher' fire_order type.
    static void _get_item_fire_order(vector<int>& order,
                                     bool ignore_inscription_etc,
                                     const item_def* launcher,
                                     bool manual)
    {
        const int inv_start = (ignore_inscription_etc ? 0
                                                      : Options.fire_items_start);

        for (int i_inv = inv_start; i_inv < ENDOFPACK; i_inv++)
        {
            const item_def& item = you.inv[i_inv];
            if (!item.defined())
                continue;

            const auto l = is_launched(&you, launcher, item);

            // don't swap to throwing when you run out of launcher ammo. (The
            // converse case should be ruled out by _item_matches below.)
            // TODO: (a) is this the right thing to do, and (b) should it be
            // done in _item_matches? I don't fully understand the logic there.
            if (!manual
                && _get_weapon_ammo_type(launcher) != AMMO_THROW
                && l == launch_retval::THROWN)
            {
                continue;
            }

            // =f prevents item from being in fire order.
            if (!ignore_inscription_etc
                && strstr(item.inscription.c_str(), manual ? "=F" : "=f"))
            {
                continue;
            }

            for (unsigned int i_flags = 0; i_flags < Options.fire_order.size();
                 i_flags++)
            {
                if (_item_matches(item, (fire_type) Options.fire_order[i_flags],
                                  launcher, manual)
                    || (launcher && _autoswitch_active()
                            && (launcher->link == letter_to_index('a')
                                || launcher->link == letter_to_index('b'))
                            && _autoswitch_ammo_check(item)))
                {
                    // this approach to sorting is pretty wtf
                    order.push_back((i_flags<<16) | (i_inv & 0xffff));
                    break;
                }
            }
        }

        sort(order.begin(), order.end());

        for (unsigned int i = 0; i < order.size(); i++)
            order[i] &= 0xffff;
    }

    /**
     * An ammo_action is an action that fires ammo from a slot in the
     * inventory. This covers both launcher-based firing, and throwing.
     */
    struct ammo_action : public action
    {
        // it could be simpler to have a distinct type for launcher ammo and
        // throwing ammo
        ammo_action(int slot=-1) : action(), ammo_slot(slot)
        {
        }

        void save(CrawlHashTable &save_target) const override; // defined below

        bool equals(const action &other) const override
        {
            // type ensured in base class
            return ammo_slot == static_cast<const ammo_action&>(other).ammo_slot;
        }

        virtual bool launcher_check() const
        {
            if (ammo_slot < 0)
                return false;
            return _item_matches(you.inv[ammo_slot], (fire_type) 0xffff,
                you.weapon(), false);
        }

        virtual bool is_enabled() const override
        {
            if (!is_valid())
                return false;

            if (fire_warn_if_impossible(true))
                return false;

            if (!launcher_check())
                return false;

            const item_def *weapon = you.weapon();
            const item_def& ammo = you.inv[ammo_slot];

            // disable if there's a no-fire inscription on ammo
            // maybe this should just be skipped altogether for this case?
            // or prompt on trigger..
            return check_warning_inscriptions(ammo, OPER_FIRE)
                && (!weapon
                    || is_launched(&you, weapon, ammo) != launch_retval::LAUNCHED
                    || check_warning_inscriptions(*weapon, OPER_FIRE));
        }

        virtual bool is_valid() const override
        {
            if (you.species == SP_FELID)
                return false;
            if (ammo_slot < 0 || ammo_slot >= ENDOFPACK)
                return false;
            const item_def& ammo = you.inv[ammo_slot];
            if (!ammo.defined())
                return false;

            if (_autoswitch_active())
            {
                // valid but potentially disabled. It seems like there could be
                // better ways of doing this given generalized quivers?
                return _autoswitch_ammo_check(ammo);
            }
            else
            {
                const item_def *weapon = you.weapon();
                return _item_matches(ammo, (fire_type) 0xffff, // TODO: ...
                                     weapon, false);
            }
        }

        bool is_targeted() const override
        {
            return !you.confused();
        }

        bool uses_mp() const override
        {
            return is_pproj_active();
        }

        void trigger(dist &t) override
        {
            target = t;
            if (!is_valid())
                return;
            if (!is_enabled())
            {
                // try autoswitching in case that's why it's disabled
                if (!_autoswitch_to_ranged(you.inv[ammo_slot]))
                    fire_warn_if_impossible(); // for messaging (TODO refactor; message about inscriptions?)
                return;
            }
            if (autofight_check())
                return;

            bolt beam;
            throw_it(beam, ammo_slot, &target);

            // TODO: eliminate this?
            you.m_quiver_history.on_item_fired(you.inv[ammo_slot], true);

            t = target; // copy back, in case they are different
        }

        virtual formatted_string quiver_description(bool short_desc) const override
        {
            ASSERT_RANGE(ammo_slot, -1, ENDOFPACK);
            // or error?
            if (!is_valid())
                return action::quiver_description(short_desc);

            formatted_string qdesc;

            const item_def& quiver = you.inv[ammo_slot];
            ASSERT(quiver.link != NON_ITEM);
            // TODO: or just lightgrey?
            qdesc.textcolour(Options.status_caption_colour);
            const launch_retval projected = is_launched(&you, you.weapon(),
                                                                    quiver);
            if (!short_desc)
            {
                string verb = you.confused() ? "confused " : "";
                switch (projected)
                {
                    case launch_retval::FUMBLED:  verb += "toss (no damage)";  break;
                    case launch_retval::LAUNCHED: verb += "fire";  break;
                    case launch_retval::THROWN:   verb += "throw"; break;
                    case launch_retval::BUGGY:    verb += "bug";   break;
                }
                qdesc.cprintf("%s: ", uppercase_first(verb).c_str());
            }

            // TODO: I don't actually know what this prefix stuff is
            const string prefix = item_prefix(quiver);

            const int prefcol =
                menu_colour(quiver.name(DESC_PLAIN), prefix, "stats");
            if (!is_enabled())
                qdesc.textcolour(DARKGREY);
            else if (prefcol != -1)
                qdesc.textcolour(prefcol);
            else
                qdesc.textcolour(LIGHTGREY);

            if (short_desc && quiver.sub_type == MI_SLING_BULLET)
            {
                qdesc.cprintf("%d bullet%s", quiver.quantity,
                                quiver.quantity > 1 ? "s" : "");
            }
            else
                qdesc += quiver.name(DESC_PLAIN, true);

            return qdesc;
        }

        int get_item() const override
        {
            return ammo_slot;
        }

        shared_ptr<action> find_replacement() const override
        {
            return find_action_from_launcher(you.weapon());
        }

        vector<shared_ptr<action>> get_fire_order(bool allow_disabled=true) const override
        {
            vector<int> fire_order;
            _get_item_fire_order(fire_order, false, you.weapon(), true);

            vector<shared_ptr<action>> result;

            for (auto i : fire_order)
            {
                auto a = make_shared<ammo_action>(i);
                if (a->is_valid() && (allow_disabled || a->is_enabled()))
                    result.push_back(move(a));
            }
            return result;
        }

    protected:
        int ammo_slot;
    };

    // for fumble throwing / tossing
    struct fumble_action : public ammo_action
    {
        fumble_action(int slot=-1) : ammo_action(slot)
        {
        }

        void save(CrawlHashTable &save_target) const override; // defined below

        // uses ammo_action fire order

        bool launcher_check() const override
        {
            return true;
        }

        bool is_valid() const override
        {
            if (you.species == SP_FELID)
                return false;
            if (ammo_slot < 0 || ammo_slot >= ENDOFPACK)
                return false;
            const item_def& ammo = you.inv[ammo_slot];
            if (!ammo.defined())
                return false;

            // slightly weird looking, but this ensures that only tossing
            // is allowed with this class. (I guess in principle it could be
            // doable to let this class toss anything, but I'm not going to
            // do that.)
            if (ammo_action::is_valid())
                return false;
            return true;
        }
    };

    static bool _spell_needs_manual_targeting(spell_type s)
    {
        switch (s)
        {
        case SPELL_FULMINANT_PRISM:
        case SPELL_GRAVITAS: // will autotarget to a monster if allowed, should we allow?
        case SPELL_PASSWALL: // targeted, but doesn't make sense with autotarget
        case SPELL_GOLUBRIAS_PASSAGE: // targeted, but doesn't make sense with autotarget
            return true;
        default:
            return false;
        }
    }

    // for spells that are targeted, but should skip the lua target selection
    // pass for one reason or another
    static bool _spell_autotarget_incompatible(spell_type s)
    {
        // XX perhaps all spells should just use direction chooser target
        // selection? This is how automagic.lua handled it.
        auto h = find_spell_targeter(s, 100, LOS_RADIUS); // dummy values
        // use smarter direction chooser target selection for spells that have
        // explosition or cloud patterning, like fireball. This allows them
        // to autoselect targets at the edge of their range, which autofire
        // wouldn't handle.
        if (!Options.simple_targeting && h && h->can_affect_outside_range())
            return true;

        switch (s)
        {
        case SPELL_LRD: // skip initial autotarget for LRD so that it doesn't
                        // fix on a close monster that can't be targeted. I'm
                        // not quite sure what the right thing to do is?
                        // An alternative would be to just error if the closest
                        // monster can't be autotargeted, or pop out to manual
                        // targeting for that case; the behavior involved in
                        // listing it here just finds the closest targetable
                        // monster.
        case SPELL_INVISIBILITY: // targeted, but not to enemies. (Should this allow quivering at all?)
        case SPELL_APPORTATION: // Apport doesn't target monsters at all
            return true;
        default:
            return _spell_needs_manual_targeting(s);
        }
    }

    struct spell_action : public action
    {
        spell_action(spell_type s = SPELL_NO_SPELL) : spell(s) { };
        void save(CrawlHashTable &save_target) const override; // defined below

        bool equals(const action &other) const override
        {
            // type ensured in base class
            return spell == static_cast<const spell_action&>(other).spell;
        }

        bool is_dynamic_targeted() const
        {
            // TODO: what spells does this miss?
            return !!(get_spell_flags(spell) & spflag::targeting_mask);
        }

        bool is_enabled() const override
        {
            return can_cast_spells(true) && !spell_is_useless(spell, true, false);
        }

        bool is_valid() const override
        {
            return is_valid_spell(spell) && you.has_spell(spell);
        }

        bool is_targeted() const override
        {
            return is_dynamic_targeted() || spell_has_targeter(spell);
        }

        bool allow_autofight() const override
        {
            return is_dynamic_targeted()
                                && !_spell_autotarget_incompatible(spell);
        }

        bool uses_mp() const override
        {
            return is_valid();
        }

        void trigger(dist &t) override
        {
            // note: we don't do the enabled check here, because cast_a_spell
            // duplicates it and does appropriate messaging
            // TODO refactor?
            if (!is_valid())
                return;

            target = t;

            // TODO: how to handle these in the fire interface?
            if (_spell_needs_manual_targeting(spell))
            {
                target.target = coord_def(-1,-1);
                target.find_target = false; // default, but here for clarity's sake
                target.interactive = true;
            }
            else if (_spell_autotarget_incompatible(spell))
            {
                target.target = coord_def(-1,-1);
                target.find_target = true;
            }
            else if (!is_dynamic_targeted())
                target.target = you.pos(); // hax -- never trigger static targeters
                                           // unless interactive is set.
                                           // will need to be fixed if `z` ever
                                           // calls here

            // don't do the range check check if doing manual firing. (It's
            // a bit hacky to condition this on whether there's a fire context...)
            const bool do_range_check = !target.fire_context;
            if (autofight_check())
                return;

            cast_a_spell(do_range_check, spell, &target);
            if (target.find_target && !target.isValid && !target.fire_context)
            {
                // It would be entirely possible to force manual targeting for
                // this case; I think it's not what players would expect so I'm
                // not doing it for now.
                // TODO: more consistency with autofight.lua messaging?
                mpr("Can't find an automatic target! Use Z to cast.");
            }
            t = target; // copy back, in case they are different
        }

        int quiver_color() const override
        {
            int col = failure_rate_colour(spell);
            // this imposes excommunication colors
            col = spell_highlight_by_utility(spell, col, true, false);
            if (!is_enabled())
                col = COL_USELESS;
            return col;
        }

        formatted_string quiver_description(bool short_desc) const override
        {
            if (!is_valid())
                return action::quiver_description(short_desc);

            formatted_string qdesc;

            qdesc.textcolour(Options.status_caption_colour);
            qdesc.cprintf("Cast: ");

            qdesc.textcolour(quiver_color());

            // TODO: is showing the spell letter useful?
            qdesc.cprintf("%s", spell_title(spell));
            if (spell == SPELL_SANDBLAST)
                qdesc.cprintf(" (stones: %d)", sandblast_find_ammo().first);

            if (fail_severity(spell) > 0)
            {
                qdesc.cprintf(" (%s)",
                        failure_rate_to_string(raw_spell_fail(spell)).c_str());
            }

            return qdesc;
        }

        vector<shared_ptr<action>> get_fire_order(bool allow_disabled=true) const override
        {
            // goes by letter order
            vector<shared_ptr<action>> result;
            for (int i = 0; i < 52; i++)
            {
                const char letter = index_to_letter(i);
                const spell_type s = get_spell_by_letter(letter);
                auto a = make_shared<spell_action>(s);
                if (a->is_valid()
                    && (allow_disabled || a->is_enabled())
                    // some extra stuff for fire order in particular: don't
                    // cycle to spells that are dangerous to past or forbidden.
                    // These can still be force-quivered.
                    && fail_severity(s) < Options.fail_severity_to_quiver
                    && spell_highlight_by_utility(s, COL_UNKNOWN) != COL_FORBIDDEN)
                {
                    result.push_back(move(a));
                }
            }
            return result;
        }

    private:
        spell_type spell;
    };

    // stuff that is silly to quiver. Basically four (overlapping) cases:
    // * one-off things that are implemented as abilities because that's what
    //   you do with random stuff that doesn't fit neatly into any triggerable
    //   type. E.g. abandon religion, choose ancestor type, etc.
    // * "stop X" type abilities, these just clutter up the list
    // * capstone abilities + stuff with a significant cost (revivify etc)
    // * abilities that vanish when triggered. E.g. fly *might* make more sense
    //   if it was a toggle, but that's not how it's implemented
    static bool _pseudoability(ability_type a)
    {
        if (   static_cast<int>(a) >= ABIL_FIRST_SACRIFICE
                    && static_cast<int>(a) <= ABIL_FINAL_SACRIFICE
            || static_cast<int>(a) >= ABIL_HEPLIAKLQANA_FIRST_TYPE
                    && static_cast<int>(a) <= ABIL_HEPLIAKLQANA_LAST_TYPE)
        {
            return true;
        }

        switch (a)
        {
        case ABIL_END_TRANSFORMATION:
        case ABIL_CANCEL_PPROJ:
        case ABIL_EXSANGUINATE:
        case ABIL_REVIVIFY:
        case ABIL_EVOKE_TURN_VISIBLE:
        case ABIL_ZIN_DONATE_GOLD:
        case ABIL_TSO_BLESS_WEAPON:
        case ABIL_KIKU_BLESS_WEAPON:
        case ABIL_KIKU_GIFT_NECRONOMICON:
        case ABIL_SIF_MUNA_FORGET_SPELL:
        case ABIL_LUGONU_BLESS_WEAPON:
        case ABIL_BEOGH_GIFT_ITEM:
        case ABIL_ASHENZARI_CURSE:
        case ABIL_RU_REJECT_SACRIFICES:
        case ABIL_HEPLIAKLQANA_IDENTITY:
        case ABIL_STOP_RECALL:
        case ABIL_RENOUNCE_RELIGION:
        case ABIL_CONVERT_TO_BEOGH:
        // not entirely pseudo, but doesn't make a lot of sense to quiver:
        case ABIL_FLY:
        case ABIL_TRAN_BAT:
            return true;
        default:
            return false;
        }
    }

    struct ability_action : public action
    {
        ability_action(ability_type a = ABIL_NON_ABILITY) : ability(a) { };
        void save(CrawlHashTable &save_target) const override; // defined below

        bool equals(const action &other) const override
        {
            // type ensured in base class
            return ability == static_cast<const ability_action&>(other).ability;
        }

        bool is_valid() const override
        {
            if (ability == ABIL_NON_ABILITY || ability == NUM_ABILITIES)
                return false;
            // it's quite something that this vector needs to be reconstructed
            // every time...
            vector<talent> talents = your_talents(false, true);
            for (const auto &t : talents)
                if (t.which == ability)
                    return true;
            return false;
        }

        bool is_enabled() const override
        {
            // TODO: _check_ability_dangerous?
            return is_valid() && check_ability_possible(ability, true);
        }

        bool is_targeted() const override
        {
            // hard-coded list of abilities that have a targeter
            // there is no general way of getting this?
            // TODO: implement static targeters for relevant abilities (which
            // is pretty much everything)
            switch (ability)
            {
            case ABIL_HOP:
            case ABIL_ROLLING_CHARGE:
            case ABIL_SPIT_POISON:
            case ABIL_BREATHE_ACID:
            case ABIL_BREATHE_FIRE:
            case ABIL_BREATHE_FROST:
            case ABIL_BREATHE_POISON:
            case ABIL_BREATHE_POWER:
            case ABIL_BREATHE_STEAM:
            case ABIL_BREATHE_MEPHITIC:
            case ABIL_DAMNATION:
            case ABIL_ZIN_IMPRISON:
            case ABIL_MAKHLEB_MINOR_DESTRUCTION:
            case ABIL_MAKHLEB_MAJOR_DESTRUCTION:
            case ABIL_LUGONU_BANISH:
            case ABIL_BEOGH_SMITING:
            case ABIL_DITHMENOS_SHADOW_STEP:
            case ABIL_QAZLAL_UPHEAVAL:
            case ABIL_RU_POWER_LEAP:
            case ABIL_USKAYAW_LINE_PASS:
            case ABIL_USKAYAW_GRAND_FINALE:
            case ABIL_WU_JIAN_WALLJUMP:
                return true;
            default:
                return false;
            }
        }

        bool allow_autofight() const override
        {
            return false;
        }

        bool uses_mp() const override
        {
            return ability_mp_cost(ability) > 0;
        }

        void trigger(dist &t) override
        {
            if (!is_valid())
                return;

            if (!is_enabled())
            {
                check_ability_possible(ability, false);
                return;
            }
            if (autofight_check())
                return;

            target = t;
            target.find_target = true;
            talent tal = get_talent(ability, false);
            activate_talent(tal, &target);

            target = t;

            t = target; // copy back, in case they are different
        }

        formatted_string quiver_description(bool short_desc) const override
        {
            if (!is_valid())
                return action::quiver_description(short_desc);

            formatted_string qdesc;

            qdesc.textcolour(Options.status_caption_colour);
            qdesc.cprintf("Abil: ");

            qdesc.textcolour(quiver_color());
            qdesc.cprintf("%s", ability_name(ability));

            return qdesc;
        }

        vector<shared_ptr<action>> get_fire_order(bool allow_disabled=true) const override
        {
            vector<talent> talents = your_talents(false, true);
            // goes by letter order
            vector<shared_ptr<action>> result;

            // TODO: all sorts of random chaff that shouldn't be in fire order
            for (const auto &tal : talents)
            {
                if (_pseudoability(tal.which))
                    continue;
                auto a = make_shared<ability_action>(tal.which);
                if (a->is_valid() && (allow_disabled || a->is_enabled()))
                    result.push_back(move(a));
            }
            return result;
        }

    private:
        ability_type ability;
    };


    // TODO: generalize to misc evokables? Code should be similar if targeting
    // can be easily implemented
    struct wand_action : public action
    {
        wand_action(int slot=-1) : action(), wand_slot(slot)
        {
        }

        virtual void save(CrawlHashTable &save_target) const override; // defined below

        bool equals(const action &other) const override
        {
            // type ensured in base class
            return wand_slot == static_cast<const wand_action&>(other).wand_slot;
        }

        bool is_enabled() const override
        {
            return evoke_check(wand_slot, true);
        }

        virtual bool is_valid() const override
        {
            if (wand_slot < 0 || wand_slot >= ENDOFPACK)
                return false;
            const item_def& wand = you.inv[wand_slot];
            if (!wand.defined() || wand.base_type != OBJ_WANDS)
                return false;
            return true;
        }

        bool is_targeted() const override
        {
            return true;
        }

        // TOOD: uses_mp for wand mp mutation? Because this mut no longer forces
        // mp use, the result is somewhat weird

        void trigger(dist &t) override
        {
            target = t;
            if (!is_valid())
                return;

            if (!is_enabled())
            {
                evoke_check(wand_slot); // for messaging
                return;
            }

            if (autofight_check())
                return;

            // to apply smart targeting behavior for iceblast; should have no
            // impact on other wands
            target.find_target = true;
            evoke_item(wand_slot, &target);

            t = target; // copy back, in case they are different
        }

        virtual string quiver_verb() const
        {
            return "Zap";
        }

        virtual formatted_string quiver_description(bool short_desc) const override
        {
            ASSERT_RANGE(wand_slot, -1, ENDOFPACK);
            if (!is_valid())
                return action::quiver_description(short_desc);
            formatted_string qdesc;

            const item_def& quiver = you.inv[wand_slot];
            ASSERT(quiver.link != NON_ITEM);
            qdesc.textcolour(Options.status_caption_colour);
            qdesc.cprintf("%s: ", quiver_verb().c_str());

            qdesc.textcolour(quiver_color());
            qdesc += quiver.name(DESC_PLAIN, true);
            return qdesc;
        }

        int get_item() const override
        {
            return wand_slot;
        }

        virtual vector<shared_ptr<action>> get_fire_order(bool allow_disabled=true) const override
        {
            // go by pack order
            vector<shared_ptr<action>> result;
            for (int slot = 0; slot < ENDOFPACK; slot++)
            {
                auto w = make_shared<wand_action>(slot);
                if (w->is_valid()
                    && (allow_disabled || w->is_enabled())
                    // skip digging for fire cycling, it seems kind of
                    // non-useful? Can still be force-quivered from inv
                    && you.inv[slot].sub_type != WAND_DIGGING)
                {
                    result.push_back(move(w));
                }
            }
            return result;
        }

    protected:
        int wand_slot;
    };

    static bool _misc_needs_manual_targeting(int subtype)
    {
            // autotargeting seems less useful on the others. Maybe this should
            // be configurable somehow?
        return subtype != MISC_PHIAL_OF_FLOODS;
    }

    struct misc_action : public wand_action
    {
        misc_action(int slot=-1) : wand_action(slot)
        {
        }

        void save(CrawlHashTable &save_target) const override; // defined below

        bool is_valid() const override
        {
            if (wand_slot < 0 || wand_slot >= ENDOFPACK)
                return false;
            const item_def& wand = you.inv[wand_slot];
            // MISC_ZIGGURAT is valid (so can be force quivered) but is skipped
            // in the fire order
            if (!wand.defined() || wand.base_type != OBJ_MISCELLANY)
                return false;
            return true;
        }

        // equals should work without override

        bool allow_autofight() const override
        {
            // all of these use the spell direction chooser
            return false;
        }

        void trigger(dist &t) override
        {
            if (is_valid()
                && _misc_needs_manual_targeting(you.inv[wand_slot].sub_type))
            {
                t.interactive = true;
            }
            wand_action::trigger(t);
        }

        string quiver_verb() const override
        {
            ASSERT(is_valid());
            switch (you.inv[wand_slot].sub_type)
            {
            case MISC_TIN_OF_TREMORSTONES:
                return "Throw";
            case MISC_HORN_OF_GERYON:
                return "Blow";
            case MISC_BOX_OF_BEASTS:
                return "Open";
            default:
                return "Evoke";
            }
        }

        bool is_targeted() const override
        {
            if (!is_valid())
                return false;
            switch (you.inv[wand_slot].sub_type)
            {
            case MISC_PHIAL_OF_FLOODS:
            case MISC_LIGHTNING_ROD:
            case MISC_PHANTOM_MIRROR:
                return true;
            default:
                return false;
            }
        }

        vector<shared_ptr<action>> get_fire_order(bool allow_disabled=true) const override
        {
            // go by pack order
            vector<shared_ptr<action>> result;
            for (int slot = 0; slot < ENDOFPACK; slot++)
            {
                auto w = make_shared<misc_action>(slot);
                if (w->is_valid()
                    && (allow_disabled || w->is_enabled())
                    && you.inv[slot].sub_type != MISC_ZIGGURAT)
                {
                    result.push_back(move(w));
                }
            }
            return result;
        }
    };

    struct artefact_evoke_action : public wand_action
    {
        artefact_evoke_action(int slot=-1) : wand_action(slot)
        {
        }

        // equals should work without override

        void save(CrawlHashTable &save_target) const override; // defined below

        bool is_valid() const override
        {
            if (wand_slot < 0 || wand_slot >= ENDOFPACK)
                return false;

            const item_def& item = you.inv[wand_slot];
            if (!item.defined() || !is_unrandom_artefact(item)
                                || !item_is_equipped(item))
            {
                return false;
            }

            const unrandart_entry *entry = get_unrand_entry(item.unrand_idx);

            if (!entry || !(entry->evoke_func || entry->targeted_evoke_func))
                return false;

            return true;
        }

        // TODO: there's no generic API for this, and it would be a pain to
        // add one...and there's only three of these. So we do a bit of dumb
        // code duplication. Maybe this could eventually be moved into
        // evoke_check?
        bool artefact_evoke_check(bool quiet) const
        {
            if (!is_valid())
                return false;

            switch (you.inv[wand_slot].unrand_idx)
            {
            case UNRAND_DISPATER:
                return enough_hp(14, quiet) && enough_mp(4, quiet); // TODO: code duplication...
            case UNRAND_OLGREB:
                return enough_mp(4, quiet); // TODO: code duplication...
            default:
                return true; // UNRAND_ASMODEUS has no up-front cost
            }
        }

        bool is_enabled() const override
        {
            return artefact_evoke_check(true);
        }

        bool allow_autofight() const override
        {
            // all of these use the spell direction chooser
            return false;
        }

        bool is_targeted() const override
        {
            if (!is_valid())
                return false;
            // is_valid checks the preconditions for this:
            return get_unrand_entry(you.inv[wand_slot].unrand_idx)->targeted_evoke_func;
        }

        string quiver_verb() const override
        {
            return "Evoke";
        }

        void trigger(dist &t) override
        {
            target = t;
            if (!is_valid())
                return;

            if (!artefact_evoke_check(false))
                return;

            if (autofight_check())
                return;

            target.find_target = true;
            evoke_item(wand_slot, &target);

            t = target; // copy back, in case they are different
        }


        vector<shared_ptr<action>> get_fire_order(bool allow_disabled=true) const override
        {
            // go by pack order
            vector<shared_ptr<action>> result;
            for (int slot = 0; slot < ENDOFPACK; slot++)
            {
                auto w = make_shared<artefact_evoke_action>(slot);
                if (w->is_valid()
                    && (allow_disabled || w->is_enabled()))
                {
                    result.push_back(move(w));
                }
            }
            return result;
        }
    };


    void action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "action";
    }

    void ammo_action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "ammo_action";
        save_target["param"] = ammo_slot;
    }

    void fumble_action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "fumble_action";
        save_target["param"] = ammo_slot;
    }

    void spell_action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "spell_action";
        save_target["param"] = static_cast<int>(spell);
    }

    void ability_action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "ability_action";
        save_target["param"] = static_cast<int>(ability);
    }

    void wand_action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "wand_action";
        save_target["param"] = static_cast<int>(wand_slot);
    }

    void misc_action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "misc_action";
        save_target["param"] = static_cast<int>(wand_slot);
    }

    void artefact_evoke_action::save(CrawlHashTable &save_target) const
    {
        save_target["type"] = "artefact_evoke_action";
        save_target["param"] = static_cast<int>(wand_slot);
    }

    static shared_ptr<action> _load_action(CrawlHashTable &source)
    {
        // pretty minimal: but most actions that I can think of shouldn't need
        // a lot of effort to save. Something to tell you the type, and a
        // single value that is usually more or less an int. Using a hashtable
        // here is future proofing.

        // save compat (or bug compat): initialize to an invalid action if we
        // are missing the keys altogether
        if (!source.exists("type") || !source.exists("param"))
            return make_shared<ammo_action>(-1);

        const string &type = source["type"].get_string();
        const int param = source["param"].get_int();

        // is there something more elegant than this?
        if (type == "ammo_action")
            return make_shared<ammo_action>(param);
        else if (type == "spell_action")
            return make_shared<spell_action>(static_cast<spell_type>(param));
        else if (type == "ability_action")
            return make_shared<ability_action>(static_cast<ability_type>(param));
        else if (type == "wand_action")
            return make_shared<wand_action>(param);
        else if (type == "misc_action")
            return make_shared<misc_action>(param);
        else if (type == "artefact_evoke_action")
            return make_shared<artefact_evoke_action>(param);
        else if (type == "fumble_action")
            return make_shared<fumble_action>(param);
        else
            return make_shared<action>();
    }

    shared_ptr<action> find_action_from_launcher(const item_def *item)
    {
        // Felids have no use for launchers or ammo.
        if (you.species == SP_FELID)
        {
            auto result = make_shared<ammo_action>(-1);
            result->error = "You can't grasp things well enough to shoot them.";
            return result;
        }

        int slot = -1;

        const int cur_launcher_item = you.launcher_action.get().get_item();
        const int cur_quiver_item = you.quiver_action.get().get_item();

        if (cur_launcher_item >= 0 && you.inv[cur_launcher_item].defined()
            && _item_matches(you.inv[cur_launcher_item], FIRE_LAUNCHER, item, false))
        {
            // prefer to keep the current ammo if not changing weapon types
            slot = cur_launcher_item;
        }
        else if (cur_quiver_item >= 0 && you.inv[cur_quiver_item].defined()
            && _item_matches(you.inv[cur_quiver_item], FIRE_LAUNCHER, item, false))
        {
            // if the right item type is currently present in the main quiver,
            // use that
            slot = cur_quiver_item;
        }
        else
        {
            // otherwise, find the last fired ammo for this launcher. (This is
            // an awful lot of effort to choose correctly between stones and
            // bullets...)
            slot = you.m_quiver_history.get_last_ammo(item);
        }

        // Finally, try looking at the fire order.
        if (slot == -1)
        {
            vector<int> order;
            _get_item_fire_order(order, false, item, false);
            if (!order.empty())
                slot = order[0];
        }

        auto result = make_shared<ammo_action>(slot);

        // if slot is still -1, we have failed, and the fire order is
        // empty for some reason. We should therefore populate the `error`
        // field for result.
        if (slot == -1)
        {
            vector<int> full_fire_order;
            _get_item_fire_order(full_fire_order, true, item, false);

            if (full_fire_order.empty())
                result->error = "No suitable missiles.";
            else
            {
                const int skipped_item = full_fire_order[0];
                if (skipped_item < Options.fire_items_start)
                {
                    result->error = make_stringf(
                        "Nothing suitable (fire_items_start = '%c').",
                        index_to_letter(Options.fire_items_start));
                }
                else
                {
                    result->error = make_stringf(
                        "Nothing suitable (ignored '=f'-inscribed item on '%c').",
                        index_to_letter(skipped_item));
                }
            }
        }

        return result;
    }

    action_cycler::action_cycler() : current(make_shared<ammo_action>(-1)) { };

    void action_cycler::save(const string key) const
    {
        auto &target = you.props[key].get_table();
        get().save(target);
    }

    void action_cycler::load(const string key)
    {
        if (!you.props.exists(key))
        {
            // some light save compat: if there is no prop, attempt to fill
            // in the quiver from whatever is wielded -- will select launcher
            // ammo if applicable, or throwing.
            set(find_action_from_launcher(you.weapon()));
            if (!get().is_valid())
                cycle();
            save(key);
        }

        auto &target = you.props[key].get_table();
        set(_load_action(target));
        // in case this is invalid, cycle. TODO: is this the right thing to do?
        on_actions_changed();
    }

    bool action_cycler::set(const shared_ptr<action> new_act)
    {
        auto n = new_act ? new_act : make_shared<action>();

        const bool diff = *n != get();
        current = n;
        if (diff)
        {
            // side effects, ugh. Update the fire history, and play a sound
            // if needed. TODO: refactor so this is less side-effect-y
            // somehow?
            const int item_slot = get().get_item();
            if (item_slot >= 0 && you.inv[item_slot].defined())
            {
                const item_def item = you.inv[item_slot];

                quiver::launcher t = quiver::AMMO_THROW;
                const item_def *weapon = you.weapon();
                if (weapon && item.launched_by(*weapon))
                    t = quiver::_get_weapon_ammo_type(weapon);

                you.m_quiver_history.set_quiver(you.inv[item_slot], t);
            }
#ifdef USE_SOUND
            parse_sound(CHANGE_QUIVER_SOUND);
#endif
        }
        set_needs_redraw();
        return diff;
    }

    static bool _is_currently_launched_ammo(int slot)
    {
        const item_def *weapon = you.weapon();
        return weapon && slot >= 0 && you.inv[slot].defined()
                                        && you.inv[slot].launched_by(*weapon);
    }

    // only reacts to ammo launched by the current weapon, or empty quiver
    // note that the action may still be valid on its own terms when this
    // returns true...
    bool launcher_action_cycler::is_empty() const
    {
        if (action_cycler::is_empty())
            return true;
        return !_is_currently_launched_ammo(get().get_item());
    }

    bool launcher_action_cycler::set(const shared_ptr<action> new_act)
    {
        if (_is_currently_launched_ammo(new_act->get_item())
            || *new_act == action())
        {
            return action_cycler::set(new_act);
        }
        else
            set_needs_redraw();
        return false;
    }

    void launcher_action_cycler::set_needs_redraw()
    {
        action_cycler::set_needs_redraw();
        you.wield_change = true;
    }

    bool action_cycler::set(const action_cycler &other)
    {
        const bool diff = current != other.current;
        // don't use regular set: avoid all the side effects when importing
        // from another action cycler. (Used in targeting.)
        current = other.current;
        set_needs_redraw();
        return diff;
    }

    // pitfall: if you do not use this return value by reference, polymorphism
    // will fail and you will end up with an action(). Easiest way to make
    // this mistake: `auto a = you.quiver_action.get()`.
    // (TODO: something to avoid this? Work with a shared_ptr after all?)
    action &action_cycler::get() const
    {
        // TODO: or find an action?
        ASSERT(current);

        return *current;
    }

    bool action_cycler::spell_is_quivered(spell_type s) const
    {
        // validity check??
        return get() == spell_action(s);
    }

    bool action_cycler::item_is_quivered(int item_slot) const
    {
        return item_slot >= 0 && item_slot < ENDOFPACK
                              && get().get_item() == item_slot;
    }

    static shared_ptr<action> _get_next_action_type(shared_ptr<action> a, int dir, bool allow_disabled)
    {
        // this all seems a bit messy

        // Construct the type order.
        vector<shared_ptr<action>> action_types;
        action_types.push_back(make_shared<ammo_action>(-1));
        action_types.push_back(make_shared<wand_action>(-1));
        action_types.push_back(make_shared<misc_action>(-1));
        action_types.push_back(make_shared<artefact_evoke_action>(-1));
        action_types.push_back(make_shared<spell_action>(SPELL_NO_SPELL));
        action_types.push_back(make_shared<ability_action>(ABIL_NON_ABILITY));

        if (dir < 0)
            reverse(action_types.begin(), action_types.end());

        size_t i = 0;
        // skip_first: true just in case the current action is valid and we
        // need to move on from it.
        bool skip_first = true;

        if (!a)
            skip_first = false; // and i = 0
        else
        {
            // find the type of a
            auto &a_ref = *a;
            for (i = 0; i < action_types.size(); i++)
            {
                auto rep = action_types[i];
                if (!rep) // should be impossible
                    continue;
                // we do it this way in order to silence some clang warnings
                auto &rep_ref = *rep;
                if (typeid(rep_ref) == typeid(a_ref))
                    break;
            }
            // unknown action type -- treat it like null. (Handles `action`.)
            if (i >= action_types.size())
            {
                i = 0;
                skip_first = false;
            }
        }

        // TODO: this logic could probably be reimplemented using only mod
        // math, but using rotate does make the final iteration very clean
        if (skip_first)
            i = (i + 1) % action_types.size();
        rotate(action_types.begin(), action_types.begin() + i, action_types.end());

        // now find the first result that is valid in this order. Will cycle
        // back to the current action type if nothing else works.
        for (auto result : action_types)
        {
            auto n = result->find_next(dir, allow_disabled, false);
            if (n && n->is_valid())
                return n;
        }

        // we've gone through everything -- somehow there are no valid actions,
        // not even using a
        return nullptr;
    }

    // not_null guaranteed
    shared_ptr<action> action_cycler::next(int dir, bool allow_disabled)
    {
        // first try the next action of the same type
        shared_ptr<action> result = get().find_next(dir, allow_disabled, false);
        // then, try to find a different action type
        if (!result || !result->is_valid())
            result = _get_next_action_type(get_ptr(), dir, allow_disabled);

        // no valid actions, return an (invalid) empty-quiver action
        if (!result)
            return make_shared<ammo_action>(-1);

        return result;
    }

    bool action_cycler::cycle(int dir, bool allow_disabled)
    {
        return set(next(dir, allow_disabled));
    }

    void action_cycler::on_actions_changed()
    {
        if (!get().is_valid())
        {
            auto r = get().find_replacement();
            if (r && r->is_valid())
                set(r);
            else
            {
                dprf("cycle");
                cycle();
            }
        }
        set_needs_redraw();
    }

    void action_cycler::set_needs_redraw()
    {
        // TODO: abstract from `you`
        you.redraw_quiver = true;
    }

    shared_ptr<action> slot_to_action(int slot, bool force)
    {
        if (slot < 0 || slot >= ENDOFPACK || !you.inv[slot].defined())
            return nullptr;

        // is this legacy(?) check needed? Maybe only relevant for fumble throwing?
        for (int i = EQ_MIN_ARMOUR; i <= EQ_MAX_WORN; i++)
        {
            if (you.equip[i] == slot)
            {
                mpr("You can't quiver worn items.");
                return make_shared<ammo_action>(-1);
            }
        }

        if (you.inv[slot].base_type == OBJ_WANDS)
            return make_shared<wand_action>(slot);
        else if (you.inv[slot].base_type == OBJ_MISCELLANY)
            return make_shared<misc_action>(slot);
        else if (is_unrandom_artefact(you.inv[slot]))
            return make_shared<artefact_evoke_action>(slot);

        // use ammo as the fallback -- may well end up invalid
        auto a = make_shared<ammo_action>(slot);
        if (force && (!a || !a->is_valid()))
            return make_shared<fumble_action>(slot);
        return a;
    }

    bool action_cycler::set_from_slot(int slot)
    {
        return(set(slot_to_action(slot)));
    }

    bool action_cycler::clear()
    {
        return(set(make_shared<action>()));
    }

    // note for editing this: Menu::action is defined and will take precedence
    // over quiver::action unless the quiver namespace is explicit.
    class ActionSelectMenu : public Menu
    {
    public:
        ActionSelectMenu(action_cycler &_quiver, bool _allow_empty)
            : Menu(MF_SINGLESELECT | MF_ALLOW_FORMATTING),
              cur_quiver(_quiver), allow_empty(_allow_empty)
        {
            set_tag("actions");
            action_cycle = Menu::CYCLE_TOGGLE;
            menu_action  = Menu::ACT_EXECUTE;
        }

        action_cycler &cur_quiver;
        bool allow_empty;

        bool set_to_quiver(shared_ptr<quiver::action> s)
        {
            if (s && s->is_valid()
                && (allow_empty || *s != quiver::action()))
            {
                cur_quiver.set(s);
                // a bit hacky:
                if (&cur_quiver == &you.quiver_action)
                    you.launcher_action.set(s);
                return true;
            }
            return false;
        }

    protected:
        bool _choose_from_inv()
        {
            int slot = prompt_invent_item(allow_empty
                                            ? "Quiver which item? (- for none)"
                                            : "Quiver which item?",
                                          menu_type::invlist, OSEL_ANY,
                                          OPER_QUIVER, invprompt_flag::hide_known, '-');

            if (prompt_failed(slot))
                return true;

            if (slot == PROMPT_GOT_SPECIAL)  // '-' or empty quiver
            {
                if (!allow_empty)
                    return true;

                cur_quiver.clear();
                return false;
            }

            // TODO: in failure it would be better to set the more with an
            // error instead of exiting the menu
            return !set_to_quiver(slot_to_action(slot, true));
        }

        bool _choose_from_abilities()
        {
            vector<talent> talents = your_talents(false);
            // TODO: better handling for no abilities?
            int selected = choose_ability_menu(talents);

            return selected >= 0 && selected < static_cast<int>(talents.size())
                && !set_to_quiver(make_shared<ability_action>(talents[selected].which));
        }

        bool process_key(int key) override
        {
            // TODO: some kind of view action option?
            if (allow_empty && key == '-')
            {
                set_to_quiver(make_shared<quiver::action>());
                // TODO maybe drop this messaging?
                mprf("Clearing quiver.");
                return false;
            }
            else if (key == '*')
                return _choose_from_inv(); // TODO: fumble ammo
            else if (key == '&')
            {
                const int skey = list_spells(false, false, false,
                                                    "Select a spell to quiver");
                if (skey == 0)
                    return true;
                if (isalpha(skey))
                {
                    auto s = make_shared<spell_action>(
                            static_cast<spell_type>(get_spell_by_letter(skey)));
                    return !set_to_quiver(s);
                }
                return false;
            }
            else if (key == '^')
                return _choose_from_abilities();
            return Menu::process_key(key);
        }

        virtual formatted_string calc_title() override
        {
            string s = "Quiver which action? (";
            if (allow_empty)
                s += "<w>-</w>: none, ";
            s += "<w>*</w>: full inventory, <w>&</w>: spells, <w>^</w>: abilities)";
            return formatted_string::parse_string(s);
        }
    };

    void action_cycler::target()
    {
        // This is a somewhat indirect interface that allows cycling between
        // arbitrary code paths that call a direction chooser. Because the
        // setup for direction choosers is so varied and complicated, we can't
        // implement the cycling internal to a direction chooser interface
        // (at least without a major refactor), so this UI takes the strategy
        // of rebuilding the direction chooser each time, but making it look
        // seamless from a user perspective. Each call to `do_target` leads to
        // the specific custom targeting code path for a particular action,
        // which then builds the direction chooser. The three CMD_TARGET...
        // commands below are not handled in a direction chooser, but rather
        // passed back via the `dist`. In addition, the `dist` object will
        // pass down a pointer to this that the direction chooser will use for
        // some custom prompt stuff. Obviously, it would better if this
        // message passing happened more directly, and in the long run perhaps
        // it would be possible to gradually refactor the various code paths
        // involved to support that, but for now that project is too
        // impractical, because each code path (except throwing) is called from
        // many places.
        shared_ptr<action> initial = get_ptr();
        clear_messages(); // this kind of looks better as a force clear, but
                          // for consistency with direct targeting commands,
                          // I will leave it as non-force
        msgwin_temporary_mode tmp;
        bool force_restore_initial;

        command_type what_happened = CMD_NO_CMD;
        do
        {
            flush_prev_message();
            msgwin_clear_temporary();
            force_restore_initial = false;
            auto a = do_target();

            // the point of this: if you cycle to or select some item, fire it,
            // and it becomes invalid (e.g. by using up ammo), this will try to
            // restore the initial quiver value rather than ending up with the
            // next in fire order item after the selected action
            if (!a || !a->is_valid())
                force_restore_initial = true;

            what_happened = a ? static_cast<command_type>(a->target.cmd_result)
                              : CMD_NO_CMD;

            switch (what_happened)
            {
            case CMD_TARGET_CYCLE_QUIVER_FORWARD:
                cycle(1, false);
                break;
            case CMD_TARGET_CYCLE_QUIVER_BACKWARD:
                cycle(-1, false);
                break;
            case CMD_TARGET_SELECT_ACTION:
                // choosing a disabled action here may exit the prompt
                // depending on the spell, it's a bit inconsistent.
                choose(*this, false);
                break;
            default:
                what_happened = CMD_NO_CMD; // shouldn't happen
                // fallthrough
            case CMD_FIRE:
            case CMD_NO_CMD:
                break;
            }
            if (!crawl_state.is_replaying_keys())
                flush_input_buffer(FLUSH_BEFORE_COMMAND);
            // right now this resets targeting on cycle; would it be better to
            // save it?
        } while (what_happened != CMD_NO_CMD && what_happened != CMD_FIRE);

        // Restore the quiver on cancel -- backwards compatible behavior.
        // Is it really the best behavior?
        if ((what_happened == CMD_NO_CMD || force_restore_initial)
            && initial && initial->is_valid())
        {
            set(initial);
        }
    }

    // should be action_cycler method?
    void choose(action_cycler &cur_quiver, bool allow_empty)
    {
        // TODO: icons in tiles, dividers or subtitles for each category?
        ActionSelectMenu menu(cur_quiver, allow_empty);
        vector<shared_ptr<action>> actions;
        auto tmp = ammo_action(-1).get_fire_order();
        actions.insert(actions.end(), tmp.begin(), tmp.end());
        tmp = wand_action(-1).get_fire_order();
        actions.insert(actions.end(), tmp.begin(), tmp.end());
        tmp = misc_action(-1).get_fire_order();
        actions.insert(actions.end(), tmp.begin(), tmp.end());
        tmp = artefact_evoke_action(-1).get_fire_order();
        actions.insert(actions.end(), tmp.begin(), tmp.end());
        tmp = spell_action(SPELL_NO_SPELL).get_fire_order();
        actions.insert(actions.end(), tmp.begin(), tmp.end());
        tmp = ability_action(ABIL_NON_ABILITY).get_fire_order();
        actions.insert(actions.end(), tmp.begin(), tmp.end());

        menu.set_title(new MenuEntry("", MEL_TITLE));

        menu_letter hotkey;
        // What to do if everything is disabled?
        for (const auto &a : actions)
        {
            if (!a || !a->is_valid())
                continue;
            MenuEntry *me = new MenuEntry(a->quiver_description(),
                                                MEL_ITEM, 1,
                                                (int) hotkey);
            // TODO: is there a way to show formatting in menu items?
            me->colour = a->quiver_color();
            me->data = (void *) &a; // pointer to vector element - don't change the vector!
            menu.add_entry(me);
            hotkey++;
        }

        menu.on_single_selection = [&menu](const MenuEntry& item)
        {
            const shared_ptr<action> *a = static_cast<shared_ptr<action> *>(item.data);
            return !menu.set_to_quiver(*a);
        };
        menu.show();
    }

    // this class is largely legacy code -- can it be done away with?
    // or refactored to use actions.
    // TODO: auto switch to last action when swapping away from a launcher --
    // right now it goes to an ammo only in that case.
    ammo_history::ammo_history()
    {
        COMPILE_CHECK(ARRAYSZ(m_last_used_of_type) == quiver::NUM_LAUNCHERS);
    }

    int ammo_history::get_last_ammo(const item_def *launcher) const
    {
        return get_last_ammo(quiver::_get_weapon_ammo_type(launcher));
    }

    int ammo_history::get_last_ammo(quiver::launcher type) const
    {
        const int slot = _get_pack_slot(m_last_used_of_type[type]);
        ASSERT(slot < ENDOFPACK && (slot == -1 || you.inv[slot].defined()));
        return slot;
    }

    void ammo_history::set_quiver(const item_def &item, quiver::launcher ammo_type)
    {
        m_last_used_of_type[ammo_type] = item;
        m_last_used_of_type[ammo_type].quantity = 1;
        you.redraw_quiver = true;
    }

    // Notification that item was fired
    void ammo_history::on_item_fired(const item_def& item, bool explicitly_chosen)
    {
        if (!explicitly_chosen)
        {
            // If the item was not actively chosen, i.e. just automatically
            // passed into the quiver, don't change any of the quiver settings.
            you.redraw_quiver = true;
            return;
        }
        // If item matches the launcher, put it in that launcher's last-used item.
        // Otherwise, it goes into last hand-thrown item.

        const item_def *weapon = you.weapon();

        if (weapon && item.launched_by(*weapon))
        {
            const quiver::launcher t = quiver::_get_weapon_ammo_type(weapon);
            m_last_used_of_type[t] = item;
            m_last_used_of_type[t].quantity = 1;    // 0 makes it invalid :(
        }
        else
        {
            const launch_retval projected = is_launched(&you, you.weapon(),
                                                        item);

            // Don't do anything if this item is not really fit for throwing.
            if (projected == launch_retval::FUMBLED)
                return;

            m_last_used_of_type[quiver::AMMO_THROW] = item;
            m_last_used_of_type[quiver::AMMO_THROW].quantity = 1;
        }

        you.redraw_quiver = true;
    }

    // ----------------------------------------------------------------------
    // Save/load
    // ----------------------------------------------------------------------

    // this save/load code is extremely legacy
    static const short QUIVER_COOKIE = short(0xb015);
    void ammo_history::save(writer& outf) const
    {
        // TODO: action-based marshalling/unmarshalling. But we will still need
        // the history here, probably.
        marshallShort(outf, QUIVER_COOKIE);

        marshallItem(outf, item_def()); // was: m_last_weapon
        marshallInt(outf, 0); // was: m_last_used_type
        marshallInt(outf, ARRAYSZ(m_last_used_of_type));

        for (unsigned int i = 0; i < ARRAYSZ(m_last_used_of_type); i++)
            marshallItem(outf, m_last_used_of_type[i]);
    }

    void ammo_history::load(reader& inf)
    {
        // warning: this is called in the unmarshalling sequence before the
        // inventory is actually in place
        const short cooky = unmarshallShort(inf);
        ASSERT(cooky == QUIVER_COOKIE); (void)cooky;

        auto dummy = item_def();
        unmarshallItem(inf, dummy); // was: m_last_weapon
        unmarshallInt(inf); // was: m_last_used_type;

        const unsigned int count = unmarshallInt(inf);
        ASSERT(count <= ARRAYSZ(m_last_used_of_type));

        for (unsigned int i = 0; i < count; i++)
            unmarshallItem(inf, m_last_used_of_type[i]);
    }

    void on_actions_changed()
    {
        you.quiver_action.on_actions_changed();
        you.launcher_action.on_actions_changed();
    }

    // Called when the player has switched weapons
    void on_weapon_changed()
    {
        const item_def* weapon = you.weapon();
        you.launcher_action.set(quiver::find_action_from_launcher(weapon));

        if (!you.launcher_action.is_empty())
        {
            // If the launcher has valid ammo, set that to the main quiver as
            // well. TODO: is this too annoying? It is based on previous
            // behavior, and is relatively intuitive in simple cases. But it
            // could be pretty annoying in a char using both spells and ranged
            // weapons. Maybe add an option?
            you.quiver_action.set(you.launcher_action.get_ptr());
        }

        // if switching invalidates the quiver, and the new weapon is an
        // evokable randart, use that action. (If someone ever makes an
        // evokable launcher, its ammo will be prioritized, revisit.) This
        // isn't as aggressive as the launcher case.
        if (weapon && is_unrandom_artefact(*weapon)
                                    && !you.quiver_action.get().is_valid())
        {
            you.quiver_action.set(
                            make_shared<artefact_evoke_action>(weapon->link));
        }

    }
}


// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

// Helper for _get_fire_order.
// Types may actually contain more than one fire_type.
static bool _item_matches(const item_def &item, fire_type types,
                          const item_def* launcher, bool manual)
{
    ASSERT(item.defined());

    if (types & FIRE_INSCRIBED)
        if (item.inscription.find(manual ? "+F" : "+f", 0) != string::npos)
            return true;

    if (item.base_type != OBJ_MISSILES)
        return false;

    if ((types & FIRE_STONE) && item.sub_type == MI_STONE)
        return true;
    if ((types & FIRE_JAVELIN) && item.sub_type == MI_JAVELIN)
        return true;
    if ((types & FIRE_ROCK) && item.sub_type == MI_LARGE_ROCK)
        return true;
    if ((types & FIRE_NET) && item.sub_type == MI_THROWING_NET)
        return true;
    if ((types & FIRE_BOOMERANG) && item.sub_type == MI_BOOMERANG)
        return true;
    if ((types & FIRE_DART) && item.sub_type == MI_DART)
        return true;

    if (types & FIRE_LAUNCHER)
    {
        if (launcher && item.launched_by(*launcher))
            return true;
    }

    return false;
}

// Returns inv slot that contains an item that looks like item,
// or -1 if not in inv.
static int _get_pack_slot(const item_def& item)
{
    if (!item.defined())
        return -1;

    if (in_inventory(item) && _items_similar(item, you.inv[item.link], false))
        return item.link;

    // First try to find the exact same item.
    for (int i = 0; i < ENDOFPACK; i++)
    {
        const item_def &inv_item = you.inv[i];
        if (inv_item.quantity && _items_similar(item, inv_item, false))
            return i;
    }

    // If that fails, try to find an item sufficiently similar.
    for (int i = 0; i < ENDOFPACK; i++)
    {
        const item_def &inv_item = you.inv[i];
        if (inv_item.quantity && _items_similar(item, inv_item, true))
        {
            // =f prevents item from being in fire order.
            if (strstr(inv_item.inscription.c_str(), "=f"))
                return -1;

            return i;
        }
    }

    return -1;
}

static bool _items_similar(const item_def& a, const item_def& b, bool force)
{
    return items_similar(a, b) && (force || a.slot == b.slot);
}
