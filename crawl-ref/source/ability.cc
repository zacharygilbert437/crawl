/**
 * @file
 * @brief Functions related to special abilities.
**/

#include "AppHdr.h"

#include "ability.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "abyss.h"
#include "act-iter.h"
#include "areas.h"
#include "artefact.h"
#include "art-enum.h"
#include "branch.h"
#include "chardump.h"
#include "cleansing-flame-source-type.h"
#include "cloud.h"
#include "coordit.h"
#include "database.h"
#include "decks.h"
#include "delay.h"
#include "describe.h"
#include "directn.h"
#include "dungeon.h"
#include "evoke.h"
#include "exercise.h"
#include "fight.h"
#include "god-abil.h"
#include "god-companions.h"
#include "god-conduct.h"
#include "god-passive.h"
#include "hints.h"
#include "invent.h"
#include "item-prop.h"
#include "items.h"
#include "item-use.h"
#include "level-state-type.h"
#include "libutil.h"
#include "macro.h"
#include "maps.h"
#include "menu.h"
#include "message.h"
#include "mon-place.h"
#include "mon-util.h"
#include "movement.h"
#include "mutation.h"
#include "notes.h"
#include "options.h"
#include "output.h"
#include "player-stats.h"
#include "potion.h"
#include "prompt.h"
#include "religion.h"
#include "rltiles/tiledef-icons.h"
#include "skills.h"
#include "spl-book.h"
#include "spl-cast.h"
#include "spl-clouds.h"
#include "spl-damage.h"
#include "spl-goditem.h"
#include "spl-miscast.h"
#include "spl-other.h"
#include "spl-selfench.h"
#include "spl-summoning.h"
#include "spl-transloc.h"
#include "stairs.h"
#include "state.h"
#include "stepdown.h"
#include "stringutil.h"
#include "tag-version.h"
#include "target.h"
#include "terrain.h"
#include "tilepick.h"
#include "transform.h"
#include "traps.h"
#include "uncancel.h"
#include "unicode.h"
#include "view.h"
#include "wiz-dgn.h"

enum class abflag
{
    none                = 0x00000000,
    breath              = 0x00000001, // ability uses DUR_BREATH_WEAPON
    delay               = 0x00000002, // ability has its own delay
    pain                = 0x00000004, // ability must hurt player (ie torment)
    piety               = 0x00000008, // ability has its own piety cost
    exhaustion          = 0x00000010, // fails if you.exhausted
    instant             = 0x00000020, // doesn't take time to use
    conf_ok             = 0x00000040, // can use even if confused
    variable_mp         = 0x00000080, // costs a variable amount of MP
    curse               = 0x00000100, // Destroys a cursed item
    max_hp_drain        = 0x00000200, // drains max hit points
    gold                = 0x00000400, // costs gold
    sacrifice           = 0x00000800, // sacrifice (Ru)
    hostile             = 0x00001000, // failure summons a hostile (Makhleb)
    berserk_ok          = 0x00002000, // can use even if berserk
    card                = 0x00004000, // deck drawing (Nemelex)
    quiet_fail          = 0x00008000, // no message on failure
};
DEF_BITFIELD(ability_flags, abflag);

struct generic_cost
{
    int base, add, rolls;

    generic_cost(int num)
        : base(num), add(num == 0 ? 0 : (num + 1) / 2 + 1), rolls(1)
    {
    }
    generic_cost(int num, int _add, int _rolls = 1)
        : base(num), add(_add), rolls(_rolls)
    {
    }
    static generic_cost fixed(int fixed)
    {
        return generic_cost(fixed, 0, 1);
    }
    static generic_cost range(int low, int high, int _rolls = 1)
    {
        return generic_cost(low, high - low + 1, _rolls);
    }

    int cost() const PURE;

    operator bool () const { return base > 0 || add > 0; }
};

struct scaling_cost
{
    int value;

    scaling_cost(int permille) : value(permille) {}

    static scaling_cost fixed(int fixed)
    {
        return scaling_cost(-fixed);
    }

    int cost(int max) const;

    operator bool () const { return value != 0; }
};

/// What affects the failure chance of the ability?
enum class fail_basis
{
    xl,
    evo,
    invo,
};

/**
 * What skill is used to determine the player's god's invocations' failure
 * chance?
 *
 * XXX: deduplicate this with the similar code for divine titles, etc
 * (skills.cc:skill_title_by_rank)
 *
 * IMPORTANT NOTE: functions that depend on this will be wrong if you aren't
 * currently worshipping a god that grants the given ability (e.g. in ?/A)!
 *
 * @return      The appropriate skill type; e.g. SK_INVOCATIONS.
 */
skill_type invo_skill(god_type god)
{
    switch (god)
    {
        case GOD_KIKUBAAQUDGHA:
            return SK_NECROMANCY;

#if TAG_MAJOR_VERSION == 34
        case GOD_PAKELLAS:
            return SK_EVOCATIONS;
#endif
        case GOD_ASHENZARI:
        case GOD_JIYVA:
        case GOD_GOZAG:
        case GOD_RU:
        case GOD_TROG:
        case GOD_WU_JIAN:
        case GOD_VEHUMET:
        case GOD_XOM:
        case GOD_IGNIS:
            return SK_NONE; // ugh
        default:
            return SK_INVOCATIONS;
    }
}

/// How to determine the odds of the ability failing?
struct failure_info
{
    /// what determines the variable portion of failure: e.g. xl, evo, invo
    fail_basis basis;
    /// base failure chance
    int base_chance;
    /// multiplier to skill/xl; subtracted from base fail chance
    int variable_fail_mult;
    /// denominator to piety; subtracted from base fail chance if invo
    int piety_fail_denom;

    /**
     * What's the chance of the ability failing if the player tries to use it
     * right now?
     *
     * See spl-cast.cc:_get_true_fail_rate() for details on what this 'chance'
     * actually means.
     *
     * @return  A failure chance; may be outside the 0-100 range.
     */
    int chance() const
    {
        switch (basis)
        {
        case fail_basis::xl:
            return base_chance - you.experience_level * variable_fail_mult;
        case fail_basis::evo:
            return base_chance - you.skill(SK_EVOCATIONS, variable_fail_mult);
        case fail_basis::invo:
        {
            const int sk_mod = invo_skill() == SK_NONE ? 0 :
                                 you.skill(invo_skill(), variable_fail_mult);
            const int piety_mod
                = piety_fail_denom ? you.piety / piety_fail_denom : 0;
            return base_chance - sk_mod - piety_mod;
        }
        default:
            die("unknown failure basis %d!", (int)basis);
        }
    }

    /// What skill governs the use of this ability, if any?
    skill_type skill() const
    {
        switch (basis)
        {
        case fail_basis::evo:
            return SK_EVOCATIONS;
        case fail_basis::invo:
            return invo_skill();
        case fail_basis::xl:
        default:
            return SK_NONE;
        }
    }
};

// Structure for representing an ability:
struct ability_def
{
    ability_type        ability;
    const char *        name;
    unsigned int        mp_cost;        // magic cost of ability
    scaling_cost        hp_cost;        // hit point cost of ability
    generic_cost        piety_cost;     // + random2((piety_cost + 1) / 2 + 1)
    failure_info        failure;        // calculator for failure odds
    ability_flags       flags;          // used for additional cost notices

    int get_mp_cost() const
    {
        if (you.has_mutation(MUT_HP_CASTING))
            return 0;
        return mp_cost;
    }

    int get_hp_cost() const
    {
        int cost = hp_cost.cost(you.hp_max);
        if (you.has_mutation(MUT_HP_CASTING))
            return cost + mp_cost;
        return cost;
    }
};

static int _lookup_ability_slot(ability_type abil);
static spret _do_ability(const ability_def& abil, bool fail, dist *target=nullptr);
static void _pay_ability_costs(const ability_def& abil);
static int _scale_piety_cost(ability_type abil, int original_cost);

static vector<ability_def> &_get_ability_list()
{
    // construct on initialization v2:
    // https://isocpp.org/wiki/faq/ctors#construct-on-first-use-v2

    // The description screen was way out of date with the actual costs.
    // This table puts all the information in one place... -- bwr
    //
    // The three numerical fields are: MP, HP, and piety.
    // Note:  piety_cost = val + random2((val + 1) / 2 + 1);
    //        hp cost is in per-mil of maxhp (i.e. 20 = 2% of hp, rounded up)
    static vector<ability_def> Ability_List =
    {
        // NON_ABILITY should always come first
        { ABIL_NON_ABILITY, "No ability", 0, 0, 0, {}, abflag::none },
        { ABIL_SPIT_POISON, "Spit Poison",
            0, 0, 0, {fail_basis::xl, 20, 1}, abflag::breath },

        { ABIL_BREATHE_FIRE, "Breathe Fire",
            0, 0, 0, {fail_basis::xl, 30, 1}, abflag::breath },
        { ABIL_BREATHE_FROST, "Breathe Frost",
            0, 0, 0, {fail_basis::xl, 30, 1}, abflag::breath },
        { ABIL_BREATHE_POISON, "Breathe Poison Gas",
            0, 0, 0, {fail_basis::xl, 30, 1}, abflag::breath },
        { ABIL_BREATHE_MEPHITIC, "Breathe Noxious Fumes",
            0, 0, 0, {fail_basis::xl, 30, 1}, abflag::breath },
        { ABIL_BREATHE_LIGHTNING, "Breathe Lightning",
            0, 0, 0, {fail_basis::xl, 30, 1}, abflag::breath },
        { ABIL_BREATHE_POWER, "Breathe Dispelling Energy",
            0, 0, 0, {fail_basis::xl, 30, 1}, abflag::breath },
        { ABIL_BREATHE_STEAM, "Breathe Steam",
            0, 0, 0, {fail_basis::xl, 20, 1}, abflag::breath },
        { ABIL_BREATHE_ACID, "Breathe Acid",
            0, 0, 0, {fail_basis::xl, 30, 1}, abflag::breath },

        { ABIL_TRAN_BAT, "Bat Form",
            2, 0, 0, {fail_basis::xl, 45, 2}, abflag::none },
        { ABIL_EXSANGUINATE, "Exsanguinate",
            0, 0, 0, {}, abflag::delay},
        { ABIL_REVIVIFY, "Revivify",
            0, 0, 0, {}, abflag::delay},
        { ABIL_DAMNATION, "Hurl Damnation",
            0, 150, 0, {fail_basis::xl, 50, 1}, abflag::none },
        { ABIL_WORD_OF_CHAOS, "Word of Chaos",
            6, 0, 0, {fail_basis::xl, 50, 1}, abflag::max_hp_drain },

        { ABIL_DIG, "Dig", 0, 0, 0, {}, abflag::instant | abflag::none },
        { ABIL_SHAFT_SELF, "Shaft Self", 0, 0, 0, {}, abflag::delay },

        { ABIL_HOP, "Hop", 0, 0, 0, {}, abflag::none },

        { ABIL_ROLLING_CHARGE, "Rolling Charge", 0, 0, 0, {}, abflag::none },
        { ABIL_BLINKBOLT, "Blinkbolt", 0, 0, 0, {}, abflag::none },

        { ABIL_HEAL_WOUNDS, "Heal Wounds",
            0, 0, 0, {fail_basis::xl, 45, 2}, abflag::none },

        // EVOKE abilities use Evocations and come from items.
        { ABIL_EVOKE_BLINK, "Evoke Blink",
            1, 0, 0, {fail_basis::evo, 40, 2}, abflag::none },
        { ABIL_EVOKE_TURN_INVISIBLE, "Evoke Invisibility",
            2, 0, 0, {fail_basis::evo, 60, 2}, abflag::max_hp_drain },

        // TODO: any way to automatically derive these from the artefact name?
        { ABIL_EVOKE_ASMODEUS, "Evoke the Sceptre of Asmodeus",
            0, 0, 0, {fail_basis::evo, 80, 3}, abflag::none },
        { ABIL_EVOKE_DISPATER, "Evoke the Staff of Dispater",
            4, 100, 0, {}, abflag::none },
        { ABIL_EVOKE_OLGREB, "Evoke the Staff of Olgreb",
            4, 0, 0, {}, abflag::none },

        { ABIL_END_TRANSFORMATION, "End Transformation",
            0, 0, 0, {}, abflag::none },

        // INVOCATIONS:
        // Zin
        { ABIL_ZIN_RECITE, "Recite",
            0, 0, 0, {fail_basis::invo, 30, 6, 20}, abflag::none },
        { ABIL_ZIN_VITALISATION, "Vitalisation",
            2, 0, 1, {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_ZIN_IMPRISON, "Imprison",
            5, 0, 4, {fail_basis::invo, 60, 5, 20}, abflag::none },
        { ABIL_ZIN_SANCTUARY, "Sanctuary",
            7, 0, 15, {fail_basis::invo, 80, 4, 25}, abflag::none },
        { ABIL_ZIN_DONATE_GOLD, "Donate Gold",
            0, 0, 0, {fail_basis::invo}, abflag::none },

        // The Shining One
        { ABIL_TSO_DIVINE_SHIELD, "Divine Shield",
            3, 0, 2, {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_TSO_CLEANSING_FLAME, "Cleansing Flame",
            5, 0, 2, {fail_basis::invo, 70, 4, 25}, abflag::none },
        { ABIL_TSO_SUMMON_DIVINE_WARRIOR, "Summon Divine Warrior",
            8, 0, 5, {fail_basis::invo, 80, 4, 25}, abflag::none },
        { ABIL_TSO_BLESS_WEAPON, "Brand Weapon With Holy Wrath", 0, 0, 0,
            {fail_basis::invo}, abflag::none },

        // Kikubaaqudgha
        { ABIL_KIKU_RECEIVE_CORPSES, "Receive Corpses",
            3, 0, 2, {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_KIKU_TORMENT, "Torment",
            4, 0, 8, {fail_basis::invo, 60, 5, 20}, abflag::none },
        { ABIL_KIKU_GIFT_CAPSTONE_SPELLS, "Receive Forbidden Knowledge", 0, 0, 0,
            {fail_basis::invo}, abflag::none },
        { ABIL_KIKU_BLESS_WEAPON, "Brand Weapon With Pain", 0, 0, 0,
            {fail_basis::invo}, abflag::pain },

        // Yredelemnul
        { ABIL_YRED_INJURY_MIRROR, "Injury Mirror",
            0, 0, 0, {fail_basis::invo, 40, 4, 20}, abflag::piety },
        { ABIL_YRED_ANIMATE_REMAINS, "Animate Remains",
            2, 0, 0, {fail_basis::invo, 40, 4, 20}, abflag::none },
        { ABIL_YRED_RECALL_UNDEAD_SLAVES, "Recall Undead Slaves",
            2, 0, 0, {fail_basis::invo, 50, 4, 20}, abflag::none },
        { ABIL_YRED_ANIMATE_DEAD, "Animate Dead",
            2, 0, 0, {fail_basis::invo, 40, 4, 20}, abflag::none },
        { ABIL_YRED_DRAIN_LIFE, "Drain Life",
            6, 0, 2, {fail_basis::invo, 60, 4, 25}, abflag::none },
        { ABIL_YRED_ENSLAVE_SOUL, "Enslave Soul",
            8, 0, 4, {fail_basis::invo, 80, 4, 25}, abflag::none },

        // Okawaru
        { ABIL_OKAWARU_HEROISM, "Heroism",
            2, 0, 1, {fail_basis::invo, 30, 6, 20}, abflag::none },
        { ABIL_OKAWARU_FINESSE, "Finesse",
            5, 0, 3, {fail_basis::invo, 60, 4, 25}, abflag::none },
        { ABIL_OKAWARU_DUEL, "Duel",
            7, 0, 8, {fail_basis::invo, 80, 4, 20}, abflag::none },

        // Makhleb
        { ABIL_MAKHLEB_MINOR_DESTRUCTION, "Minor Destruction",
            0, scaling_cost::fixed(1), 0, {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_MAKHLEB_LESSER_SERVANT_OF_MAKHLEB, "Lesser Servant of Makhleb",
            0, scaling_cost::fixed(4), 2, {fail_basis::invo, 40, 5, 20}, abflag::hostile },
        { ABIL_MAKHLEB_MAJOR_DESTRUCTION, "Major Destruction",
            0, scaling_cost::fixed(6), generic_cost::range(0, 1),
            {fail_basis::invo, 60, 4, 25}, abflag::none },
        { ABIL_MAKHLEB_GREATER_SERVANT_OF_MAKHLEB, "Greater Servant of Makhleb",
            0, scaling_cost::fixed(10), 5,
            {fail_basis::invo, 90, 2, 5}, abflag::hostile },

        // Sif Muna
        { ABIL_SIF_MUNA_CHANNEL_ENERGY, "Channel Magic",
            0, 0, 2, {fail_basis::invo, 60, 4, 25}, abflag::none },
        { ABIL_SIF_MUNA_FORGET_SPELL, "Forget Spell",
            0, 0, 8, {fail_basis::invo}, abflag::none },
        { ABIL_SIF_MUNA_DIVINE_EXEGESIS, "Divine Exegesis",
            0, 0, 12, {fail_basis::invo, 80, 4, 25}, abflag::none },

        // Trog
        { ABIL_TROG_BERSERK, "Berserk",
            0, 0, 1, {fail_basis::invo, 45, 0, 2}, abflag::none },
        { ABIL_TROG_HAND, "Trog's Hand",
            0, 0, 2, {fail_basis::invo, piety_breakpoint(2), 0, 1}, abflag::none },
        { ABIL_TROG_BROTHERS_IN_ARMS, "Brothers in Arms",
            0, 0, generic_cost::range(5, 6),
            {fail_basis::invo, piety_breakpoint(5), 0, 1}, abflag::none },

        // Elyvilon
        { ABIL_ELYVILON_PURIFICATION, "Purification",
            2, 0, 2, {fail_basis::invo, 20, 5, 20}, abflag::conf_ok },
        { ABIL_ELYVILON_HEAL_OTHER, "Heal Other",
            2, 0, 2, {fail_basis::invo, 40, 5, 20}, abflag::none },

        { ABIL_ELYVILON_HEAL_SELF, "Heal Self",
            2, 0, 3, {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_ELYVILON_DIVINE_VIGOUR, "Divine Vigour",
            0, 0, 6, {fail_basis::invo, 80, 4, 25}, abflag::none },

        // Lugonu
        { ABIL_LUGONU_ABYSS_EXIT, "Depart the Abyss",
            1, 0, 10, {fail_basis::invo, 30, 6, 20}, abflag::none },
        { ABIL_LUGONU_BEND_SPACE, "Bend Space",
            1, scaling_cost::fixed(2), 0, {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_LUGONU_BANISH, "Banish", 4, 0, generic_cost::range(3, 4),
            {fail_basis::invo, 85, 7, 20}, abflag::none },
        { ABIL_LUGONU_CORRUPT, "Corrupt", 7, scaling_cost::fixed(5), 10,
            {fail_basis::invo, 70, 4, 25}, abflag::none },
        { ABIL_LUGONU_ABYSS_ENTER, "Enter the Abyss", 10, 0, 28,
            {fail_basis::invo, 80, 4, 25}, abflag::pain },
        { ABIL_LUGONU_BLESS_WEAPON, "Brand Weapon With Distortion", 0, 0, 0,
            {fail_basis::invo}, abflag::none },

        // Nemelex
        { ABIL_NEMELEX_DRAW_DESTRUCTION, "Draw Destruction",
            0, 0, 0, {fail_basis::invo}, abflag::card },
        { ABIL_NEMELEX_DRAW_ESCAPE, "Draw Escape",
            0, 0, 0, {fail_basis::invo}, abflag::card },
        { ABIL_NEMELEX_DRAW_SUMMONING, "Draw Summoning",
            0, 0, 0, {fail_basis::invo}, abflag::card },
        { ABIL_NEMELEX_DRAW_STACK, "Draw Stack",
            0, 0, 0, {fail_basis::invo}, abflag::card },
        { ABIL_NEMELEX_TRIPLE_DRAW, "Triple Draw",
            2, 0, 6, {fail_basis::invo, 60, 5, 20}, abflag::none },
        { ABIL_NEMELEX_DEAL_FOUR, "Deal Four",
            8, 0, 4, {fail_basis::invo, -1}, abflag::none }, // failure special-cased
        { ABIL_NEMELEX_STACK_FIVE, "Stack Five",
            5, 0, 10, {fail_basis::invo, 80, 4, 25}, abflag::none },

        // Beogh
        { ABIL_BEOGH_SMITING, "Smiting",
            3, 0, generic_cost::range(3, 4), {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_BEOGH_RECALL_ORCISH_FOLLOWERS, "Recall Orcish Followers",
            2, 0, 0, {fail_basis::invo, 30, 6, 20}, abflag::none },
        { ABIL_BEOGH_GIFT_ITEM, "Give Item to Named Follower",
            0, 0, 0, {fail_basis::invo}, abflag::none },
        { ABIL_BEOGH_RESURRECTION, "Resurrection",
            0, 0, 28, {fail_basis::invo}, abflag::none },

        // Jiyva
        { ABIL_JIYVA_OOZEMANCY, "Oozemancy",
            3, 0, 8, {fail_basis::invo, 80, 0, 2}, abflag::none },
        { ABIL_JIYVA_SLIMIFY, "Slimify",
            5, 0, 10, {fail_basis::invo, 90, 0, 2}, abflag::none },

        // Fedhas
        { ABIL_FEDHAS_WALL_OF_BRIARS, "Wall of Briars",
            3, 0, 2, {fail_basis::invo, 30, 6, 20}, abflag::none},
        { ABIL_FEDHAS_GROW_BALLISTOMYCETE, "Grow Ballistomycete",
            4, 0, 4, {fail_basis::invo, 60, 4, 25}, abflag::none },
        { ABIL_FEDHAS_OVERGROW, "Overgrow",
            8, 0, 12, {fail_basis::invo, 70, 5, 20}, abflag::none},
        { ABIL_FEDHAS_GROW_OKLOB, "Grow Oklob",
            6, 0, 6, {fail_basis::invo, 80, 4, 25}, abflag::none },

        // Cheibriados
        { ABIL_CHEIBRIADOS_TIME_BEND, "Bend Time",
            3, 0, 1, {fail_basis::invo, 40, 4, 20}, abflag::none },
        { ABIL_CHEIBRIADOS_DISTORTION, "Temporal Distortion",
            4, 0, 3, {fail_basis::invo, 60, 5, 20}, abflag::instant },
        { ABIL_CHEIBRIADOS_SLOUCH, "Slouch",
            5, 0, 8, {fail_basis::invo, 60, 4, 25}, abflag::none },
        { ABIL_CHEIBRIADOS_TIME_STEP, "Step From Time",
            10, 0, 10, {fail_basis::invo, 80, 4, 25}, abflag::none },

        // Ashenzari
        { ABIL_ASHENZARI_CURSE, "Curse Item",
            0, 0, 0, {fail_basis::invo}, abflag::none },
        { ABIL_ASHENZARI_UNCURSE, "Shatter the Chains",
            0, 0, 0, {fail_basis::invo}, abflag::curse },

        // Dithmenos
        { ABIL_DITHMENOS_SHADOW_STEP, "Shadow Step",
            4, 80, 5, {fail_basis::invo, 30, 6, 20}, abflag::none },
        { ABIL_DITHMENOS_SHADOW_FORM, "Shadow Form",
            9, 0, 12, {fail_basis::invo, 80, 4, 25}, abflag::max_hp_drain },

        // Ru
        { ABIL_RU_DRAW_OUT_POWER, "Draw Out Power", 0, 0, 0,
            {fail_basis::invo}, abflag::exhaustion|abflag::max_hp_drain|abflag::conf_ok },
        { ABIL_RU_POWER_LEAP, "Power Leap",
            5, 0, 0, {fail_basis::invo}, abflag::exhaustion },
        { ABIL_RU_APOCALYPSE, "Apocalypse",
            8, 0, 0, {fail_basis::invo}, abflag::exhaustion|abflag::max_hp_drain },

        { ABIL_RU_SACRIFICE_PURITY, "Sacrifice Purity",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_WORDS, "Sacrifice Words",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_DRINK, "Sacrifice Drink",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_ESSENCE, "Sacrifice Essence",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_HEALTH, "Sacrifice Health",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_STEALTH, "Sacrifice Stealth",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_ARTIFICE, "Sacrifice Artifice",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_LOVE, "Sacrifice Love",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_COURAGE, "Sacrifice Courage",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_ARCANA, "Sacrifice Arcana",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_NIMBLENESS, "Sacrifice Nimbleness",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_DURABILITY, "Sacrifice Durability",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_HAND, "Sacrifice a Hand",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_EXPERIENCE, "Sacrifice Experience",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_SKILL, "Sacrifice Skill",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_EYE, "Sacrifice an Eye",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_SACRIFICE_RESISTANCE, "Sacrifice Resistance",
            0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
        { ABIL_RU_REJECT_SACRIFICES, "Reject Sacrifices",
            0, 0, 0, {fail_basis::invo}, abflag::none },

        // Gozag
        { ABIL_GOZAG_POTION_PETITION, "Potion Petition",
            0, 0, 0, {fail_basis::invo}, abflag::gold },
        { ABIL_GOZAG_CALL_MERCHANT, "Call Merchant",
            0, 0, 0, {fail_basis::invo}, abflag::gold|abflag::none },
        { ABIL_GOZAG_BRIBE_BRANCH, "Bribe Branch",
            0, 0, 0, {fail_basis::invo}, abflag::gold },

        // Qazlal
        { ABIL_QAZLAL_UPHEAVAL, "Upheaval",
            4, 0, 3, {fail_basis::invo, 40, 5, 20}, abflag::none },
        { ABIL_QAZLAL_ELEMENTAL_FORCE, "Elemental Force",
            6, 0, 6, {fail_basis::invo, 60, 5, 20}, abflag::none },
        { ABIL_QAZLAL_DISASTER_AREA, "Disaster Area",
            7, 0, 10, {fail_basis::invo, 70, 4, 25}, abflag::none },

        // Uskayaw
        { ABIL_USKAYAW_STOMP, "Stomp",
            3, 0, generic_cost::fixed(20), {fail_basis::invo}, abflag::none },
        { ABIL_USKAYAW_LINE_PASS, "Line Pass",
            4, 0, generic_cost::fixed(20), {fail_basis::invo}, abflag::none},
        { ABIL_USKAYAW_GRAND_FINALE, "Grand Finale",
            8, 0, generic_cost::fixed(0),
            {fail_basis::invo, 120 + piety_breakpoint(4), 5, 1}, abflag::none},

        // Hepliaklqana
        { ABIL_HEPLIAKLQANA_RECALL, "Recall Ancestor",
            2, 0, 0, {fail_basis::invo}, abflag::none },
        { ABIL_HEPLIAKLQANA_TRANSFERENCE, "Transference",
            2, 0, 3, {fail_basis::invo, 40, 5, 20},
            abflag::none },
        { ABIL_HEPLIAKLQANA_IDEALISE, "Idealise",
            4, 0, 4, {fail_basis::invo, 60, 4, 25},
            abflag::none },

        { ABIL_HEPLIAKLQANA_TYPE_KNIGHT,       "Ancestor Life: Knight",
            0, 0, 0, {fail_basis::invo}, abflag::none },
        { ABIL_HEPLIAKLQANA_TYPE_BATTLEMAGE,   "Ancestor Life: Battlemage",
            0, 0, 0, {fail_basis::invo}, abflag::none },
        { ABIL_HEPLIAKLQANA_TYPE_HEXER,        "Ancestor Life: Hexer",
            0, 0, 0, {fail_basis::invo}, abflag::none },

        { ABIL_HEPLIAKLQANA_IDENTITY,  "Ancestor Identity",
            0, 0, 0, {fail_basis::invo}, abflag::instant },

        // Wu Jian
        { ABIL_WU_JIAN_SERPENTS_LASH, "Serpent's Lash",
            0, 0, 2, {fail_basis::invo}, abflag::exhaustion | abflag::instant },
        { ABIL_WU_JIAN_HEAVENLY_STORM, "Heavenly Storm",
            0, 0, 20, {fail_basis::invo, piety_breakpoint(5), 0, 1}, abflag::none },
        // Lunge and Whirlwind abilities aren't menu abilities but currently need
        // to exist for action counting, hence need enums/entries.
        { ABIL_WU_JIAN_LUNGE, "Lunge", 0, 0, 0, {}, abflag::berserk_ok },
        { ABIL_WU_JIAN_WHIRLWIND, "Whirlwind", 0, 0, 0, {}, abflag::berserk_ok },
        { ABIL_WU_JIAN_WALLJUMP, "Wall Jump",
            0, 0, 0, {}, abflag::berserk_ok },

        // Ignis
        { ABIL_IGNIS_FIERY_ARMOUR, "Fiery Armour",
            0, 0, 8, {fail_basis::invo}, abflag::none },
        { ABIL_IGNIS_FOXFIRE, "Foxfire Swarm",
            0, 0, 12, {fail_basis::invo}, abflag::quiet_fail },
        { ABIL_IGNIS_RISING_FLAME, "Rising Flame",
            0, 0, 0, {fail_basis::invo}, abflag::none },

        { ABIL_STOP_RECALL, "Stop Recall", 0, 0, 0, {fail_basis::invo}, abflag::none },
        { ABIL_RENOUNCE_RELIGION, "Renounce Religion",
            0, 0, 0, {fail_basis::invo}, abflag::none },
        { ABIL_CONVERT_TO_BEOGH, "Convert to Beogh",
            0, 0, 0, {fail_basis::invo}, abflag::none },
#ifdef WIZARD
        { ABIL_WIZ_BUILD_TERRAIN, "Build terrain",
            0, 0, 0, {}, abflag::instant },
        { ABIL_WIZ_SET_TERRAIN, "Set terrain to build",
            0, 0, 0, {}, abflag::instant },
        { ABIL_WIZ_CLEAR_TERRAIN, "Clear terrain to floor",
            0, 0, 0, {}, abflag::instant },
#endif

    };
    return Ability_List;
}

static const ability_def& get_ability_def(ability_type abil)
{
    for (const ability_def &ab_def : _get_ability_list())
        if (ab_def.ability == abil)
            return ab_def;

    return _get_ability_list()[0];
}

vector<ability_type> get_defined_abilities()
{
    vector<ability_type> r;
    for (const ability_def &ab_def : _get_ability_list())
        r.push_back(ab_def.ability);
    return r;
}

unsigned int ability_mp_cost(ability_type abil)
{
    return get_ability_def(abil).get_mp_cost();
}

/**
 * Is there a valid ability with a name matching that given?
 *
 * @param key   The name in question. (Not case sensitive.)
 * @return      true if such an ability exists; false if not.
 */
bool string_matches_ability_name(const string& key)
{
    return ability_by_name(key) != ABIL_NON_ABILITY;
}

static bool _invis_causes_drain()
{
    return !player_equip_unrand(UNRAND_INVISIBILITY);
}

/**
 * Find an ability whose name matches the given key.
 *
 * @param name      The name in question. (Not case sensitive.)
 * @return          The enum of the relevant ability, if there was one; else
 *                  ABIL_NON_ABILITY.
 */
ability_type ability_by_name(const string &key)
{
    for (const auto &abil : _get_ability_list())
    {
        if (abil.ability == ABIL_NON_ABILITY)
            continue;

        const string name = lowercase_string(ability_name(abil.ability));
        if (name == lowercase_string(key))
            return abil.ability;
    }

    return ABIL_NON_ABILITY;
}

string print_abilities()
{
    string text = "\n<w>a:</w> ";

    const vector<talent> talents = your_talents(false);

    if (talents.empty())
        text += "no special abilities";
    else
    {
        for (unsigned int i = 0; i < talents.size(); ++i)
        {
            if (i)
                text += ", ";
            text += ability_name(talents[i].which);
        }
    }

    return text;
}

int get_gold_cost(ability_type ability)
{
    switch (ability)
    {
    case ABIL_GOZAG_CALL_MERCHANT:
        return gozag_price_for_shop(true);
    case ABIL_GOZAG_POTION_PETITION:
        return GOZAG_POTION_PETITION_AMOUNT;
    case ABIL_GOZAG_BRIBE_BRANCH:
        return GOZAG_BRIBE_AMOUNT;
    default:
        return 0;
    }
}

string nemelex_card_text(ability_type ability)
{
    int cards = deck_cards(ability_deck(ability));

    if (ability == ABIL_NEMELEX_DRAW_STACK)
        return make_stringf("(next: %s)", stack_top().c_str());
    else
        return make_stringf("(%d in deck)", cards);
}

static const int VAMPIRE_BAT_FORM_STAT_DRAIN = 2;

static string _ashenzari_curse_text()
{
    const CrawlVector& curses = you.props[CURSE_KNOWLEDGE_KEY].get_vector();
    return "(Boost: "
           + comma_separated_fn(curses.begin(), curses.end(),
                                curse_abbr, "/", "/")
           + ")";
}

const string make_cost_description(ability_type ability)
{
    const ability_def& abil = get_ability_def(ability);
    string ret;
    if (abil.get_mp_cost())
        ret += make_stringf(", %d MP", abil.get_mp_cost());

    if (abil.flags & abflag::variable_mp)
        ret += ", MP";

    if (ability == ABIL_HEAL_WOUNDS)
        ret += make_stringf(", Permanent MP (%d left)", get_real_mp(false));

    if (ability == ABIL_TRAN_BAT)
    {
        ret += make_stringf(", Stat Drain (%d each)",
                            VAMPIRE_BAT_FORM_STAT_DRAIN);
    }

    if (ability == ABIL_REVIVIFY)
        ret += ", Frailty";

    if (ability == ABIL_ASHENZARI_CURSE
        && !you.props[CURSE_KNOWLEDGE_KEY].get_vector().empty())
    {
        ret += ", ";
        ret += _ashenzari_curse_text();
    }

    const int hp_cost = abil.get_hp_cost();
    if (hp_cost)
        ret += make_stringf(", %d HP", hp_cost);

    if (abil.piety_cost || abil.flags & abflag::piety)
        ret += ", Piety"; // randomised and exact amount hidden from player

    if (abil.flags & abflag::breath)
        ret += ", Breath";

    if (abil.flags & abflag::delay)
        ret += ", Delay";

    if (abil.flags & abflag::pain)
        ret += ", Pain";

    if (abil.flags & abflag::exhaustion)
        ret += ", Exhaustion";

    if (abil.flags & abflag::instant)
        ret += ", Instant"; // not really a cost, more of a bonus - bwr

    if (abil.flags & abflag::max_hp_drain
        && (ability != ABIL_EVOKE_TURN_INVISIBLE || _invis_causes_drain()))
    {
        ret += ", Max HP drain";
    }

    if (abil.flags & abflag::curse)
        ret += ", Cursed item";

    if (abil.flags & abflag::gold)
    {
        const int amount = get_gold_cost(ability);
        if (amount)
            ret += make_stringf(", %d Gold", amount);
        else if (ability == ABIL_GOZAG_POTION_PETITION)
            ret += ", Free";
        else
            ret += ", Gold";
    }

    if (abil.flags & abflag::sacrifice)
    {
        ret += ", ";
        const string prefix = "Sacrifice ";
        ret += string(ability_name(ability)).substr(prefix.size());
        ret += ru_sac_text(ability);
    }

    if (abil.flags & abflag::card)
    {
        ret += ", ";
        ret += "A Card ";
        ret += nemelex_card_text(ability);
    }

    // If we haven't output anything so far, then the effect has no cost
    if (ret.empty())
        return "None";

    ret.erase(0, 2);
    return ret;
}

static string _get_piety_amount_str(int value)
{
    return value > 15 ? "extremely large" :
           value > 10 ? "large" :
           value > 5  ? "moderate" :
                        "small";
}

static const string _detailed_cost_description(ability_type ability)
{
    const ability_def& abil = get_ability_def(ability);
    ostringstream ret;

    bool have_cost = false;
    ret << "This ability costs: ";

    if (abil.get_mp_cost())
    {
        have_cost = true;
        ret << "\nMP     : ";
        ret << abil.get_mp_cost();
    }
    if (abil.get_hp_cost())
    {
        have_cost = true;
        ret << "\nHP     : ";
        ret << abil.get_hp_cost();
    }

    if (abil.piety_cost || abil.flags & abflag::piety)
    {
        have_cost = true;
        ret << "\nPiety  : ";
        if (abil.flags & abflag::piety)
            ret << "variable";
        else
        {
            int avgcost = abil.piety_cost.base + abil.piety_cost.add / 2;
            ret << _get_piety_amount_str(avgcost);
        }
    }

    if (abil.flags & abflag::gold)
    {
        have_cost = true;
        ret << "\nGold   : ";
        int gold_amount = get_gold_cost(ability);
        if (gold_amount)
            ret << gold_amount;
        else if (ability == ABIL_GOZAG_POTION_PETITION)
            ret << "free";
        else
            ret << "variable";
    }

    if (abil.flags & abflag::curse)
    {
        have_cost = true;
        ret << "\nOne cursed item";
    }

    if (!have_cost)
        ret << "nothing.";

    if (abil.flags & abflag::breath)
        ret << "\nYou must catch your breath between uses of this ability.";

    if (abil.flags & abflag::delay)
        ret << "\nThis ability takes some time before being effective.";

    if (abil.flags & abflag::pain)
        ret << "\nUsing this ability will hurt you.";

    if (abil.flags & abflag::exhaustion)
        ret << "\nThis ability causes exhaustion, and cannot be used when exhausted.";

    if (abil.flags & abflag::instant)
        ret << "\nThis ability is instantaneous.";

    if (abil.flags & abflag::conf_ok)
        ret << "\nYou can use this ability even if confused.";

    if (abil.flags & abflag::max_hp_drain
        && (ability != ABIL_EVOKE_TURN_INVISIBLE || _invis_causes_drain()))
    {
        ret << "\nThis ability will temporarily drain your maximum hit points when used";
        if (ability == ABIL_EVOKE_TURN_INVISIBLE)
            ret << ", even unsuccessfully";
        ret << ".";
    }

    if (abil.ability == ABIL_HEAL_WOUNDS)
    {
        ASSERT(!have_cost); // validate just in case this ever changes
        return "This ability has a chance of reducing your maximum magic "
               "capacity when used.";
    }

    return ret.str();
}

// TODO: consolidate with player_has_ability?
ability_type fixup_ability(ability_type ability)
{
    switch (ability)
    {
    case ABIL_YRED_ANIMATE_REMAINS:
        // suppress animate remains once animate dead is unlocked (ugh)
        if (in_good_standing(GOD_YREDELEMNUL, 2))
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_YRED_RECALL_UNDEAD_SLAVES:
    case ABIL_BEOGH_RECALL_ORCISH_FOLLOWERS:
        if (!you.recall_list.empty())
            return ABIL_STOP_RECALL;
        return ability;

    case ABIL_TROG_BERSERK:
        if (you.is_lifeless_undead() || you.stasis())
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_EVOKE_BLINK:
        if (you.stasis())
            return ABIL_NON_ABILITY;
        else
            return ability;

    case ABIL_LUGONU_ABYSS_EXIT:
    case ABIL_LUGONU_ABYSS_ENTER:
        if (brdepth[BRANCH_ABYSS] == -1)
            return ABIL_NON_ABILITY;
        else
            return ability;

    case ABIL_OKAWARU_DUEL:
        if (brdepth[BRANCH_ARENA] == -1)
            return ABIL_NON_ABILITY;
        else
            return ability;

    case ABIL_TSO_BLESS_WEAPON:
    case ABIL_KIKU_BLESS_WEAPON:
    case ABIL_LUGONU_BLESS_WEAPON:
        if (you.has_mutation(MUT_NO_GRASPING))
            return ABIL_NON_ABILITY;
        else
            return ability;

    case ABIL_ELYVILON_HEAL_OTHER:
    case ABIL_TSO_SUMMON_DIVINE_WARRIOR:
    case ABIL_MAKHLEB_LESSER_SERVANT_OF_MAKHLEB:
    case ABIL_MAKHLEB_GREATER_SERVANT_OF_MAKHLEB:
    case ABIL_TROG_BROTHERS_IN_ARMS:
    case ABIL_GOZAG_BRIBE_BRANCH:
    case ABIL_QAZLAL_ELEMENTAL_FORCE:
        if (you.allies_forbidden())
            return ABIL_NON_ABILITY;
        else
            return ability;

    case ABIL_SIF_MUNA_CHANNEL_ENERGY:
        if (you.get_mutation_level(MUT_HP_CASTING))
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_SIF_MUNA_FORGET_SPELL:
        if (you.get_mutation_level(MUT_INNATE_CASTER))
            return ABIL_NON_ABILITY;
        return ability;

    default:
        return ability;
    }
}

/// Handle special cases for ability failure chances.
static int _adjusted_failure_chance(ability_type ability, int base_chance)
{
    switch (ability)
    {
    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_BREATHE_ACID:
    case ABIL_BREATHE_LIGHTNING:
    case ABIL_BREATHE_POWER:
    case ABIL_BREATHE_MEPHITIC:
    case ABIL_BREATHE_STEAM:
        if (you.form == transformation::dragon)
            return base_chance - 20;
        return base_chance;

    case ABIL_NEMELEX_DEAL_FOUR:
        return 70 - (you.piety * 2 / 45) - you.skill(SK_INVOCATIONS, 9) / 2;

    default:
        return base_chance;
    }
}

talent get_talent(ability_type ability, bool check_confused)
{
    ASSERT(ability != ABIL_NON_ABILITY);

    // Placeholder handling, part 1: The ability we have might be a
    // placeholder, so convert it into its corresponding ability before
    // doing anything else, so that we'll handle its flags properly.
    talent result { fixup_ability(ability), 0, 0, false };
    const ability_def &abil = get_ability_def(result.which);

    if (check_confused && you.confused()
        && !testbits(abil.flags, abflag::conf_ok))
    {
        result.which = ABIL_NON_ABILITY;
        return result;
    }

    // Look through the table to see if there's a preference, else find
    // a new empty slot for this ability. - bwr
    const int index = find_ability_slot(abil.ability);
    result.hotkey = index >= 0 ? index_to_letter(index) : 0;

    const int base_chance = abil.failure.chance();
    const int failure = _adjusted_failure_chance(ability, base_chance);
    result.fail = max(0, min(100, failure));

    result.is_invocation = abil.failure.basis == fail_basis::invo;

    return result;
}

const char* ability_name(ability_type ability)
{
    return get_ability_def(ability).name;
}

vector<const char*> get_ability_names()
{
    vector<const char*> result;
    for (const talent &tal : your_talents(false))
        result.push_back(ability_name(tal.which));
    return result;
}

static string _curse_desc()
{
    if (!you.props.exists(CURSE_KNOWLEDGE_KEY))
        return "";

    const CrawlVector& curses = you.props[CURSE_KNOWLEDGE_KEY].get_vector();

    if (curses.empty())
        return "";

    return "\nIf you bind an item with this curse Ashenzari will enhance "
           "the following skills:\n"
           + comma_separated_fn(curses.begin(), curses.end(), desc_curse_skills,
                                ".\n", ".\n") + ".";
}

static string _desc_sac_mut(const CrawlStoreValue &mut_store)
{
    return mut_upgrade_summary(static_cast<mutation_type>(mut_store.get_int()));
}

static string _sacrifice_desc(const ability_type ability)
{
    const string boilerplate =
        "\nIf you make this sacrifice, your powers granted by Ru "
        "will become stronger in proportion to the value of the "
        "sacrifice, and you may gain new powers as well.\n\n"
        "Sacrifices cannot be taken back.\n";
    const string piety_info = ru_sacrifice_description(ability);
    const string desc = boilerplate + piety_info;

    if (!you_worship(GOD_RU))
        return desc;

    const string sac_vec_key = ru_sacrifice_vector(ability);
    if (sac_vec_key.empty())
        return desc;

    ASSERT(you.props.exists(sac_vec_key));
    const CrawlVector &sacrifice_muts = you.props[sac_vec_key].get_vector();
    return "\nAfter this sacrifice, you will find that "
            + comma_separated_fn(sacrifice_muts.begin(), sacrifice_muts.end(),
                                 _desc_sac_mut)
            + ".\n" + desc;
}

static string _nemelex_desc(ability_type ability)
{
    ostringstream desc;
    deck_type deck = ability_deck(ability);

    desc << "Draw a card from " << (deck == DECK_STACK ? "your " : "the ");
    desc << deck_name(deck) << "; " << lowercase_first(deck_description(deck));

    return desc.str();
}

// XXX: should this be in describe.cc?
string get_ability_desc(const ability_type ability, bool need_title)
{
    const string& name = ability_name(ability);

    string lookup;

    if (testbits(get_ability_def(ability).flags, abflag::card))
        lookup = _nemelex_desc(ability);
    else
        lookup = getLongDescription(name + " ability");

    if (lookup.empty()) // Nothing found?
        lookup = "No description found.\n";

    if (ability == ABIL_ASHENZARI_CURSE)
        lookup += _curse_desc();

    if (testbits(get_ability_def(ability).flags, abflag::sacrifice))
        lookup += _sacrifice_desc(ability);

    ostringstream res;
    if (need_title)
        res << name << "\n\n";
    res << lookup << "\n" << _detailed_cost_description(ability);

    const string quote = getQuoteString(name + " ability");
    if (!quote.empty())
        res << "\n\n" << quote;

    return res.str();
}

static void _print_talent_description(const talent& tal)
{
    describe_ability(tal.which);
}

void no_ability_msg()
{
    // Give messages if the character cannot use innate talents right now.
    // * Vampires can't turn into bats when full of blood.
    // * Tengu can't start to fly if already flying.
    if (you.get_mutation_level(MUT_VAMPIRISM) >= 2)
    {
        if (you.transform_uncancellable)
            mpr("You can't untransform!");
        else
        {
            ASSERT(you.vampire_alive);
            mpr("Sorry, you cannot become a bat while alive.");
        }
    }
    else if (you.get_mutation_level(MUT_TENGU_FLIGHT)
             || you.get_mutation_level(MUT_BIG_WINGS))
    {
        if (you.airborne())
            mpr("You're already flying!");
    }
    else
        mpr("Sorry, you're not good enough to have a special ability.");
}

// Prompts the user for an ability to use, first checking the lua hook
// c_choose_ability
bool activate_ability()
{
    vector<talent> talents = your_talents(false);

    if (talents.empty())
    {
        no_ability_msg();
        crawl_state.zero_turns_taken();
        return false;
    }

    int selected = -1;

    string luachoice;

    if (!clua.callfn("c_choose_ability", ">s", &luachoice))
    {
        if (!clua.error.empty())
            mprf(MSGCH_ERROR, "Lua error: %s", clua.error.c_str());
    }
    else if (!luachoice.empty())
    {
        bool valid = false;
        // Sanity check
        for (unsigned int i = 0; i < talents.size(); ++i)
        {
            if (talents[i].hotkey == luachoice[0])
            {
                selected = static_cast<int>(i);
                valid = true;
                break;
            }
        }

        // Lua gave us garbage, defer to the user
        if (!valid)
            selected = -1;
    }

#ifndef TOUCH_UI
    if (Options.ability_menu && selected == -1)
#else
    if (selected == -1)
#endif
    {
        selected = choose_ability_menu(talents);
        if (selected == -1)
        {
            canned_msg(MSG_OK);
            crawl_state.zero_turns_taken();
            return false;
        }
    }
#ifndef TOUCH_UI
    else
    {
        while (selected < 0)
        {
            msg::streams(MSGCH_PROMPT) << "Use which ability? (? or * to list) "
                                       << endl;

            const int keyin = get_ch();

            if (keyin == '?' || keyin == '*')
            {
                selected = choose_ability_menu(talents);
                if (selected == -1)
                {
                    canned_msg(MSG_OK);
                    crawl_state.zero_turns_taken();
                    return false;
                }
            }
            else if (key_is_escape(keyin) || keyin == ' ' || keyin == '\r'
                     || keyin == '\n')
            {
                canned_msg(MSG_OK);
                crawl_state.zero_turns_taken();
                return false;
            }
            else if (isaalpha(keyin))
            {
                // Try to find the hotkey.
                for (unsigned int i = 0; i < talents.size(); ++i)
                {
                    if (talents[i].hotkey == keyin)
                    {
                        selected = static_cast<int>(i);
                        break;
                    }
                }

                // If we can't, cancel out.
                if (selected < 0)
                {
                    mpr("You can't do that.");
                    crawl_state.zero_turns_taken();
                    return false;
                }
            }
        }
    }
#endif
    return activate_talent(talents[selected]);
}

static bool _can_movement_ability(bool quiet)
{
    if (you.attribute[ATTR_HELD])
    {
        if (!quiet)
            mprf("You cannot do that while %s.", held_status());
        return false;
    }
    else if (you.is_stationary())
    {
        if (!quiet)
            canned_msg(MSG_CANNOT_MOVE);
        return false;
    }
    return true;
}

static bool _can_hop(bool quiet)
{
    if (you.duration[DUR_NO_HOP])
    {
        if (!quiet)
            mpr("Your legs are too worn out to hop.");
        return false;
    }
    return _can_movement_ability(quiet);
}

static bool _can_blinkbolt(bool quiet)
{
    if (you.duration[DUR_BLINKBOLT_COOLDOWN])
    {
        if (!quiet)
            mpr("You aren't ready to blinkbolt again yet.");
        return false;
    }
    return true;
}

static bool _can_rising_flame(bool quiet)
{
    ASSERT(can_do_capstone_ability(GOD_IGNIS));
    if (you.duration[DUR_RISING_FLAME])
    {
        if (!quiet)
            mpr("You're already rising!");
        return false;
    }
    if (!level_above().is_valid())
    {
        if (!quiet)
            mpr("You can't rise from this level!");
        return false;
    }
    return true;
}

// Check prerequisites for a number of abilities.
// Abort any attempt if these cannot be met, without losing the turn.
// TODO: Many more cases need to be added!
static bool _check_ability_possible(const ability_def& abil, bool quiet = false)
{
#ifdef WIZARD
    if (abil.ability >= ABIL_FIRST_WIZ)
        return you.wizard;
#endif
    if (you.berserk() && !testbits(abil.flags, abflag::berserk_ok))
    {
        if (!quiet)
            canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    // Doing these would outright kill the player.
    // (or, in the case of the stat-zeros, they'd at least be extremely
    // dangerous.)
    if (abil.ability == ABIL_END_TRANSFORMATION)
    {
        if (feat_dangerous_for_form(transformation::none, env.grid(you.pos()))
            && !you.duration[DUR_FLIGHT])
        {
            if (!quiet)
            {
                mprf("Turning back right now would cause you to %s!",
                    env.grid(you.pos()) == DNGN_LAVA ? "burn" : "drown");
            }

            return false;
        }
    }
    else if ((abil.ability == ABIL_EXSANGUINATE
              || abil.ability == ABIL_REVIVIFY)
            && you.form != transformation::none)
    {
        if (feat_dangerous_for_form(transformation::none, env.grid(you.pos())))
        {
            if (!quiet)
            {
                mprf("Becoming %s right now would cause you to %s!",
                    abil.ability == ABIL_EXSANGUINATE ? "bloodless" : "alive",
                    env.grid(you.pos()) == DNGN_LAVA ? "burn" : "drown");
            }

            return false;
        }
    }

    if (abil.ability == ABIL_TROG_BERSERK
        && !you.can_go_berserk(true, false, quiet))
    {
        return false;
    }

    if (you.confused() && !testbits(abil.flags, abflag::conf_ok))
    {
        if (!quiet)
            canned_msg(MSG_TOO_CONFUSED);
        return false;
    }

    // Silence and water elementals
    if (silenced(you.pos())
        || you.duration[DUR_WATER_HOLD] && !you.res_water_drowning())
    {
        talent tal = get_talent(abil.ability, false);
        if (tal.is_invocation && abil.ability != ABIL_RENOUNCE_RELIGION)
        {
            if (!quiet)
            {
                mprf("You cannot call out to %s while %s.",
                     god_name(you.religion).c_str(),
                     you.duration[DUR_WATER_HOLD] ? "unable to breathe"
                                                  : "silenced");
            }
            return false;
        }
        if (abil.ability == ABIL_WORD_OF_CHAOS)
        {
            if (!quiet)
            {
                mprf("You cannot speak a word of chaos while %s.",
                     you.duration[DUR_WATER_HOLD] ? "unable to breathe"
                                                  : "silenced");
            }
            return false;
        }
    }

    const god_power* god_power = god_power_from_ability(abil.ability);
    if (god_power && !god_power_usable(*god_power))
    {
        if (!quiet)
            canned_msg(MSG_GOD_DECLINES);
        return false;
    }

    if (testbits(abil.flags, abflag::card) && !deck_cards(ability_deck(abil.ability)))
    {
        if (!quiet)
            mpr("That deck is empty!");
        return false;
    }

    if (!quiet)
    {
        vector<text_pattern> &actions = Options.confirm_action;
        if (!actions.empty())
        {
            const char* name = ability_name(abil.ability);
            for (const text_pattern &action : actions)
            {
                if (action.matches(name))
                {
                    string prompt = "Really use " + string(name) + "?";
                    if (!yesno(prompt.c_str(), false, 'n'))
                    {
                        canned_msg(MSG_OK);
                        return false;
                    }
                    break;
                }
            }
        }
    }

    // Check that we can afford to pay the costs.
    // Note that mutation shenanigans might leave us with negative MP,
    // so don't fail in that case if there's no MP cost.
    if (abil.get_mp_cost() > 0 && !enough_mp(abil.get_mp_cost(), quiet, true))
        return false;

    const int hpcost = abil.get_hp_cost();
    if (hpcost > 0 && !enough_hp(hpcost, quiet))
        return false;

    switch (abil.ability)
    {
    case ABIL_ZIN_RECITE:
    {
        if (!zin_check_able_to_recite(quiet))
            return false;

        int result = zin_check_recite_to_monsters(quiet);
        if (result != 1)
        {
            if (!quiet)
            {
                if (result == 0)
                    mpr("There's no appreciative audience!");
                else if (result == -1)
                    mpr("You are not zealous enough to affect this audience!");
            }
            return false;
        }
        return true;
    }

    case ABIL_ZIN_SANCTUARY:
        if (env.sanctuary_time)
        {
            if (!quiet)
                mpr("There's already a sanctuary in place on this level.");
            return false;
        }
        return true;

    case ABIL_ZIN_DONATE_GOLD:
        if (!you.gold)
        {
            if (!quiet)
                mpr("You have nothing to donate!");
            return false;
        }
        return true;

    case ABIL_YRED_ANIMATE_REMAINS:
        if (animate_remains(you.pos(), CORPSE_BODY, BEH_FRIENDLY, 1,
                            MHITYOU, &you, "", GOD_YREDELEMNUL, false,
                            true) <= 0)
        {
            if (!quiet)
                mpr("There is nothing here to animate!");
            return false;
        }
        return true;

    case ABIL_YRED_ANIMATE_DEAD:
        if (!animate_dead(&you, 1, BEH_FRIENDLY, MHITYOU, &you, "",
                          GOD_YREDELEMNUL, false))
        {
            if (!quiet)
                mpr("There is nothing nearby to animate!");
            return false;
        }
        return true;

    case ABIL_ELYVILON_HEAL_SELF:
        if (you.hp == you.hp_max)
        {
            if (!quiet)
                canned_msg(MSG_FULL_HEALTH);
            return false;
        }
        return true;

    case ABIL_ELYVILON_PURIFICATION:
        if (!you.duration[DUR_SICKNESS]
            && !you.duration[DUR_POISONING]
            && !you.duration[DUR_CONF] && !you.duration[DUR_SLOW]
            && !you.petrifying()
            && you.strength(false) == you.max_strength()
            && you.intel(false) == you.max_intel()
            && you.dex(false) == you.max_dex()
            && !player_drained()
            && !you.duration[DUR_WEAK])
        {
            if (!quiet)
                mpr("Nothing ails you!");
            return false;
        }
        return true;

    case ABIL_JIYVA_OOZEMANCY:
        if (you.duration[DUR_OOZEMANCY])
        {
            if (!quiet)
                mpr("You are already calling forth ooze!");
            return false;
        }
        return true;

    case ABIL_KIKU_TORMENT:
        if (!kiku_take_corpse(true))
        {
            if (!quiet)
                mpr("There are no nearby corpses to sacrifice!");
            return false;
        }
        return true;

    case ABIL_LUGONU_ABYSS_EXIT:
        if (!player_in_branch(BRANCH_ABYSS))
        {
            if (!quiet)
                mpr("You aren't in the Abyss!");
            return false;
        }
        return true;

    case ABIL_LUGONU_CORRUPT:
        return !is_level_incorruptible(quiet);

    case ABIL_LUGONU_ABYSS_ENTER:
        if (player_in_branch(BRANCH_ABYSS))
        {
            if (!quiet)
                mpr("You're already here!");
            return false;
        }
        return true;

    case ABIL_SIF_MUNA_FORGET_SPELL:
        if (you.spell_no == 0)
        {
            if (!quiet)
                canned_msg(MSG_NO_SPELLS);
            return false;
        }
        return true;

    case ABIL_SIF_MUNA_DIVINE_EXEGESIS:
        return can_cast_spells(quiet, true);

    case ABIL_FEDHAS_WALL_OF_BRIARS:
    {
        vector<coord_def> spaces = find_briar_spaces(true);
        if (spaces.empty())
        {
            if (!quiet)
                mpr("There isn't enough space to grow briars here.");
            return false;
        }
        return true;
    }

    case ABIL_SPIT_POISON:
    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_BREATHE_POISON:
    case ABIL_BREATHE_LIGHTNING:
    case ABIL_BREATHE_ACID:
    case ABIL_BREATHE_POWER:
    case ABIL_BREATHE_STEAM:
    case ABIL_BREATHE_MEPHITIC:
        if (you.duration[DUR_BREATH_WEAPON])
        {
            if (!quiet)
                canned_msg(MSG_CANNOT_DO_YET);
            return false;
        }
        return true;

    case ABIL_HEAL_WOUNDS:
        if (you.hp == you.hp_max)
        {
            if (!quiet)
                canned_msg(MSG_FULL_HEALTH);
            return false;
        }
        if (get_real_mp(false) < 1)
        {
            if (!quiet)
                mpr("You don't have enough innate magic capacity.");
            return false;
        }
        return true;

    case ABIL_SHAFT_SELF:
        return you.can_do_shaft_ability(quiet);

    case ABIL_HOP:
        return _can_hop(quiet);

    case ABIL_ROLLING_CHARGE:
        return _can_movement_ability(quiet) &&
                                palentonga_charge_possible(quiet, true);

    case ABIL_WORD_OF_CHAOS:
        if (you.duration[DUR_WORD_OF_CHAOS_COOLDOWN])
        {
            if (!quiet)
                canned_msg(MSG_CANNOT_DO_YET);
            return false;
        }
        return true;

    case ABIL_EVOKE_BLINK:
        if (you.duration[DUR_BLINK_COOLDOWN])
        {
            if (!quiet)
                mpr("You are still too unstable to blink.");
            return false;
        }
        // fallthrough
    case ABIL_BLINKBOLT:
    {
        const string no_tele_reason = you.no_tele_reason(true);
        if (no_tele_reason.empty())
            return true;

        if (!quiet)
             mpr(no_tele_reason);
        return false;
    }

    case ABIL_TROG_BERSERK:
        return you.can_go_berserk(true, false, true)
               && (quiet || berserk_check_wielded_weapon());

    case ABIL_EVOKE_TURN_INVISIBLE:
        if (you.duration[DUR_INVIS])
        {
            if (!quiet)
                mpr("You are already invisible!");
            return false;
        }
        return true;

    case ABIL_EVOKE_ASMODEUS:
        if (you.allies_forbidden())
        {
            if (!quiet)
                mpr("Nothing will answer your call!");
            return false;
        }
        return true;

    case ABIL_GOZAG_POTION_PETITION:
        return gozag_setup_potion_petition(quiet);

    case ABIL_GOZAG_CALL_MERCHANT:
        return gozag_setup_call_merchant(quiet);

    case ABIL_GOZAG_BRIBE_BRANCH:
        return gozag_check_bribe_branch(quiet);

    case ABIL_RU_SACRIFICE_EXPERIENCE:
        if (you.experience_level <= RU_SAC_XP_LEVELS)
        {
            if (!quiet)
                mpr("You don't have enough experience to sacrifice.");
            return false;
        }
        return true;

        // only available while your ancestor is alive.
    case ABIL_HEPLIAKLQANA_IDEALISE:
    case ABIL_HEPLIAKLQANA_RECALL:
    case ABIL_HEPLIAKLQANA_TRANSFERENCE:
        if (hepliaklqana_ancestor() == MID_NOBODY)
        {
            if (!quiet)
            {
                mprf("%s is still trapped in memory!",
                     hepliaklqana_ally_name().c_str());
            }
            return false;
        }
        return true;

    case ABIL_WU_JIAN_WALLJUMP:
    {
        // TODO: Add check for whether there is any valid landing spot
        if (you.is_nervous())
        {
            if (!quiet)
                mpr("You are too terrified to wall jump!");
            return false;
        }
        if (you.attribute[ATTR_HELD])
        {
            if (!quiet)
            {
                mprf("You cannot wall jump while caught in a %s.",
                     get_trapping_net(you.pos()) == NON_ITEM ? "web" : "net");
            }
            return false;
        }
        // Is there a valid place to wall jump?
        bool has_targets = false;
        for (adjacent_iterator ai(you.pos()); ai; ++ai)
            if (feat_can_wall_jump_against(env.grid(*ai)))
            {
                has_targets = true;
                break;
            }

        if (!has_targets)
        {
            if (!quiet)
                mpr("There is nothing to wall jump against here.");
            return false;
        }
        return true;
    }

    case ABIL_IGNIS_RISING_FLAME:
        return _can_rising_flame(quiet);

    default:
        return true;
    }
}

static bool _check_ability_dangerous(const ability_type ability,
                                     bool quiet = false)
{
    if (ability == ABIL_TRAN_BAT)
        return !check_form_stat_safety(transformation::bat, quiet);
    else if (ability == ABIL_END_TRANSFORMATION
             && !feat_dangerous_for_form(transformation::none,
                                         env.grid(you.pos())))
    {
        return !check_form_stat_safety(transformation::bat, quiet);
    }
    else
        return false;
}

bool check_ability_possible(const ability_type ability, bool quiet)
{
    return _check_ability_possible(get_ability_def(ability), quiet);
}

bool activate_talent(const talent& tal, dist *target)
{
    const ability_def& abil = get_ability_def(tal.which);

    if (_check_ability_dangerous(abil.ability) || !_check_ability_possible(abil))
    {
        crawl_state.zero_turns_taken();
        return false;
    }

    bool fail = random2avg(100, 3) < tal.fail;

    const spret ability_result = _do_ability(abil, fail, target);
    switch (ability_result)
    {
        case spret::success:
            ASSERT(!fail || testbits(abil.flags, abflag::hostile));
            practise_using_ability(abil.ability);
            _pay_ability_costs(abil);
            count_action(tal.is_invocation ? CACT_INVOKE : CACT_ABIL, abil.ability);
            return true;
        case spret::fail:
            if (!testbits(abil.flags, abflag::quiet_fail))
                mpr("You fail to use your ability.");
            you.turn_is_over = true;
            return false;
        case spret::abort:
            crawl_state.zero_turns_taken();
            return false;
        case spret::none:
        default:
            die("Weird ability return type");
            return false;
    }
}

static int _calc_breath_ability_range(ability_type ability)
{
    int range = 0;

    switch (ability)
    {
    case ABIL_BREATHE_ACID:
        range = 3;
        break;
    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_SPIT_POISON:
        range = 5;
        break;
    case ABIL_BREATHE_MEPHITIC:
    case ABIL_BREATHE_STEAM:
    case ABIL_BREATHE_POISON:
        range = 6;
        break;
    case ABIL_BREATHE_LIGHTNING:
    case ABIL_BREATHE_POWER:
        range = LOS_MAX_RANGE;
        break;
    default:
        die("Bad breath type!");
        break;
    }

    return min((int)you.current_vision, range);
}

static bool _acid_breath_can_hit(const actor *act)
{
    if (act->is_monster())
    {
        const monster* mons = act->as_monster();
        bolt testbeam;
        testbeam.thrower = KILL_YOU;
        zappy(ZAP_BREATHE_ACID, 100, false, testbeam);

        return !testbeam.ignores_monster(mons);
    }
    else
        return false;
}

/// If the player is stationary, print 'You cannot move.' and return true.
static bool _abort_if_stationary()
{
    if (!you.is_stationary())
        return false;

    canned_msg(MSG_CANNOT_MOVE);
    return true;
}

static bool _cleansing_flame_affects(const actor *act)
{
    return act->res_holy_energy() < 3;
}

static string _vampire_str_int_info_blurb(string stats_affected)
{
    return make_stringf("This will reduce your %s to zero. ",
                        stats_affected.c_str());
}

/*
 * Create a string which informs the player of the consequences of bat form.
 *
 * @param str_affected Whether the player will cause strength stat zero by
 * Bat Form's stat drain ability cost.
 * @param dex_affected Whether the player will cause dexterity stat zero by
 * Bat Form's stat drain ability cost, disregarding Bat Form's dexterity boost.
 * @param int_affected Whether the player will cause intelligence stat zero by
 * Bat Form's stat drain ability cost.
 * @returns The string prompt to give the player.
 */
static string _vampire_bat_transform_prompt(bool str_affected, bool dex_affected,
                                            bool intel_affected)
{
    string prompt = "";

    if (str_affected && intel_affected)
        prompt += _vampire_str_int_info_blurb("strength and intelligence");
    else if (str_affected)
        prompt += _vampire_str_int_info_blurb("strength");
    else if (intel_affected)
        prompt += _vampire_str_int_info_blurb("intelligence");

    // Bat form's dexterity boost will keep a vampire's dexterity above zero until
    // they untransform.
    if (dex_affected)
        prompt += "This will reduce your dexterity to zero once you untransform. ";

    prompt += "Continue?";

    return prompt;
}

static bool _stat_affected_by_bat_form_stat_drain(int stat_value)
{
    // We check whether the stat is greater than zero to avoid prompting if a
    // stat is already zero.
    return 0 < stat_value && stat_value <= VAMPIRE_BAT_FORM_STAT_DRAIN;
}

/*
 * Give the player a chance to cancel a bat form transformation which could
 * cause their stats to be drained to zero.
 *
 * @returns Whether the player canceled the transformation.
 */
static bool _player_cancels_vampire_bat_transformation()
{

    bool str_affected = _stat_affected_by_bat_form_stat_drain(you.strength());
    bool dex_affected = _stat_affected_by_bat_form_stat_drain(you.dex());
    bool intel_affected = _stat_affected_by_bat_form_stat_drain(you.intel());

    // Don't prompt if there's no risk of stat-zero
    if (!str_affected && !dex_affected && !intel_affected)
        return false;

    string prompt = _vampire_bat_transform_prompt(str_affected, dex_affected,
                                                  intel_affected);

    bool proceed_with_transformation = yesno(prompt.c_str(), false, 'n');

    if (!proceed_with_transformation)
        canned_msg(MSG_OK);

    return !proceed_with_transformation;
}

static void _cause_vampire_bat_form_stat_drain()
{
    lose_stat(STAT_STR, VAMPIRE_BAT_FORM_STAT_DRAIN);
    lose_stat(STAT_INT, VAMPIRE_BAT_FORM_STAT_DRAIN);
    lose_stat(STAT_DEX, VAMPIRE_BAT_FORM_STAT_DRAIN);
}

static void _evoke_sceptre_of_asmodeus()
{
    const monster_type mon = random_choose_weighted(
                                   3, MONS_BALRUG,
                                   2, MONS_HELLION,
                                   1, MONS_BRIMSTONE_FIEND);

    mgen_data mg(mon, BEH_CHARMED, you.pos(), MHITYOU,
                 MG_FORCE_BEH, you.religion);
    mg.set_summoned(&you, 0, 0);
    mg.extra_flags |= (MF_NO_REWARD | MF_HARD_RESET);

    monster *m = create_monster(mg);

    if (m)
    {
        mpr("The sceptre summons one of its terrible servants. It is charmed, for now...");

        m->add_ench(mon_enchant(ENCH_FAKE_ABJURATION, 6));

        did_god_conduct(DID_EVIL, 3);
    }
    else
        mpr("The air shimmers briefly.");
}

static bool _evoke_staff_of_dispater(dist *target)
{
    int power = you.skill(SK_EVOCATIONS, 8);

    if (your_spells(SPELL_HURL_DAMNATION, power, false, nullptr, target)
        == spret::abort)
    {
        return false;
    }
    mpr("You feel the staff feeding on your energy!");
    return true;
}

static bool _evoke_staff_of_olgreb(dist *target)
{
    int power = div_rand_round(20 + you.skill(SK_EVOCATIONS, 20), 4);

    if (your_spells(SPELL_OLGREBS_TOXIC_RADIANCE, power, false, nullptr, target)
        == spret::abort)
    {
        return false;
    }
    did_god_conduct(DID_WIZARDLY_ITEM, 10);
    return true;
}

/*
 * Use an ability.
 *
 * @param abil The actual ability used.
 * @param fail If true, the ability is doomed to fail, and spret::fail will
 * be returned if the ability is not spret::aborted.
 * @returns Whether the spell succeeded (spret::success), failed (spret::fail),
 *  or was canceled (spret::abort). Never returns spret::none.
 */
static spret _do_ability(const ability_def& abil, bool fail, dist *target)
{
    dist target_local;
    if (!target)
        target = &target_local;

    bolt beam;

    // Note: the costs will not be applied until after this switch
    // statement... it's assumed that only failures have returned! - bwr
    switch (abil.ability)
    {
    case ABIL_HEAL_WOUNDS:
        fail_check();
        if (one_chance_in(4))
        {
            mpr("Your magical essence is drained by the effort!");
            rot_mp(1);
        }
        potionlike_effect(POT_HEAL_WOUNDS, 40);
        break;

    case ABIL_DIG:
        fail_check();
        if (!you.digging)
        {
            you.digging = true;
            mpr("You extend your mandibles.");
        }
        else
        {
            you.digging = false;
            mpr("You retract your mandibles.");
        }
        break;

    case ABIL_SHAFT_SELF:
        fail_check();
        if (you.can_do_shaft_ability(false))
        {
            if (cancel_harmful_move())
                return spret::abort;

            if (yesno("Are you sure you want to shaft yourself?", true, 'n'))
                start_delay<ShaftSelfDelay>(1);
            else
                return spret::abort;
        }
        else
            return spret::abort;
        break;

    case ABIL_HOP:
        if (_can_hop(false))
            return frog_hop(fail, target);
        else
            return spret::abort;

    case ABIL_ROLLING_CHARGE:
        if (_can_movement_ability(false))
            return palentonga_charge(fail, target);
        else
            return spret::abort;


    case ABIL_BLINKBOLT:
        {
            if (_can_blinkbolt(false))
            {
                int power = 0;
                if (you.props.exists(AIRFORM_POWER_KEY))
                    power = you.props[AIRFORM_POWER_KEY].get_int();
                else
                    return spret::abort;
                return your_spells(SPELL_BLINKBOLT, power, false, nullptr, target);
            }
            else
                return spret::abort;
        }


    case ABIL_SPIT_POISON:      // Naga poison spit
    {
        int power = 10 + you.experience_level;
        beam.range = _calc_breath_ability_range(abil.ability);

        if (!spell_direction(*target, beam)
            || !player_tracer(ZAP_SPIT_POISON, power, beam))
        {
            return spret::abort;
        }
        else
        {
            fail_check();
            zapping(ZAP_SPIT_POISON, power, beam);
            you.set_duration(DUR_BREATH_WEAPON, 3 + random2(5));
        }
        break;
    }

    case ABIL_BREATHE_ACID:       // Draconian acid splash
    {
        int pow = (you.form == transformation::dragon) ?
            2 * you.experience_level : you.experience_level;
        beam.range = _calc_breath_ability_range(abil.ability);
        targeter_splash hitfunc(&you, beam.range, pow);
        direction_chooser_args args;
        args.mode = TARG_HOSTILE;
        args.hitfunc = &hitfunc;
        if (!spell_direction(*target, beam, &args))
            return spret::abort;

        if (stop_attack_prompt(hitfunc, "spit at", _acid_breath_can_hit))
            return spret::abort;

        fail_check();
        zapping(ZAP_BREATHE_ACID, pow, beam, false, "You spit a glob of acid.");

        you.increase_duration(DUR_BREATH_WEAPON,
                          3 + random2(10) + random2(30 - you.experience_level));
        break;
    }

    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_BREATHE_POISON:
    case ABIL_BREATHE_POWER:
    case ABIL_BREATHE_STEAM:
    case ABIL_BREATHE_MEPHITIC:
        beam.range = _calc_breath_ability_range(abil.ability);
        if (!spell_direction(*target, beam))
            return spret::abort;

        // fallthrough to ABIL_BREATHE_LIGHTNING

    case ABIL_BREATHE_LIGHTNING: // not targeted
        fail_check();

        // TODO: refactor this to use only one call to zapping(), don't
        // duplicate its fail_check(), split out breathe_lightning, etc

        switch (abil.ability)
        {
        case ABIL_BREATHE_FIRE:
        {
            int power = you.experience_level;

            if (you.form == transformation::dragon)
                power += 12;

            string msg = "You breathe a blast of fire";
            msg += (power < 15) ? '.' : '!';

            if (zapping(ZAP_BREATHE_FIRE, power, beam, true, msg.c_str())
                == spret::abort)
            {
                return spret::abort;
            }
            break;
        }

        case ABIL_BREATHE_FROST:
            if (zapping(ZAP_BREATHE_FROST,
                        you.form == transformation::dragon
                            ? 2 * you.experience_level : you.experience_level,
                        beam, true, "You exhale a wave of freezing cold.")
                == spret::abort)
            {
                return spret::abort;
            }
            break;

        case ABIL_BREATHE_POISON:
            if (zapping(ZAP_BREATHE_POISON, you.experience_level, beam, true,
                        "You exhale a blast of poison gas.")
                == spret::abort)
            {
                return spret::abort;
            }
            break;

        case ABIL_BREATHE_LIGHTNING:
            mpr("You breathe a wild blast of lightning!");
            black_drac_breath();
            break;

        case ABIL_BREATHE_POWER:
            if (zapping(ZAP_BREATHE_POWER,
                        you.form == transformation::dragon
                            ? 2 * you.experience_level : you.experience_level,
                        beam, true, "You breathe a bolt of dispelling energy.")
                == spret::abort)
            {
                return spret::abort;
            }
            break;

        case ABIL_BREATHE_STEAM:
            if (zapping(ZAP_BREATHE_STEAM,
                        you.form == transformation::dragon
                            ? 2 * you.experience_level : you.experience_level,
                        beam, true, "You exhale a blast of scalding steam.")
                == spret::abort)
            {
                return spret::abort;
            }
            break;

        case ABIL_BREATHE_MEPHITIC:
            if (zapping(ZAP_BREATHE_MEPHITIC,
                        you.form == transformation::dragon
                            ? 2 * you.experience_level : you.experience_level,
                        beam, true, "You exhale a blast of noxious fumes.")
                == spret::abort)
            {
                return spret::abort;
            }
            break;

        default:
            break;
        }

        you.increase_duration(DUR_BREATH_WEAPON,
                      3 + random2(10) + random2(30 - you.experience_level));

        if (abil.ability == ABIL_BREATHE_STEAM)
            you.duration[DUR_BREATH_WEAPON] /= 2;

        break;

    case ABIL_EVOKE_BLINK:      // randarts
        return cast_blink(min(50, 1 + you.skill(SK_EVOCATIONS, 3)), fail);

    case ABIL_EVOKE_ASMODEUS:
        fail_check();
        _evoke_sceptre_of_asmodeus();
        break;

    case ABIL_EVOKE_DISPATER:
        if (!_evoke_staff_of_dispater(target))
            return spret::abort;
        break;

    case ABIL_EVOKE_OLGREB:
        if (!_evoke_staff_of_olgreb(target))
            return spret::abort;
        break;

    // DEMONIC POWERS:
    case ABIL_DAMNATION:
        fail_check();
        if (your_spells(SPELL_HURL_DAMNATION,
                        40 + you.experience_level * 6,
                        false, nullptr, target) == spret::abort)
        {
            return spret::abort;
        }
        break;

    case ABIL_WORD_OF_CHAOS:
        return word_of_chaos(40 + you.experience_level * 6, fail);

    case ABIL_EVOKE_TURN_INVISIBLE:     // cloaks, randarts
        if (!invis_allowed())
            return spret::abort;
        if (_invis_causes_drain())
            drain_player(60, false, true); // yes, before the fail check!
        fail_check();
        potionlike_effect(POT_INVISIBILITY,
                          player_adjust_evoc_power(
                              you.skill(SK_EVOCATIONS, 2) + 5));
        contaminate_player(1000 + random2(500), true);
        break;

    case ABIL_END_TRANSFORMATION:
        fail_check();
        untransform();
        break;

    // INVOCATIONS:
    case ABIL_ZIN_RECITE:
    {
        fail_check();
        if (zin_check_recite_to_monsters() == 1)
        {
            you.attribute[ATTR_RECITE_TYPE] = (recite_type) random2(NUM_RECITE_TYPES); // This is just flavor
            you.attribute[ATTR_RECITE_SEED] = random2(2187); // 3^7
            you.duration[DUR_RECITE] = 3 * BASELINE_DELAY;
            mprf("You clear your throat and prepare to recite.");
        }
        else
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;
    }
    case ABIL_ZIN_VITALISATION:
        fail_check();
        zin_vitalisation();
        break;

    case ABIL_ZIN_IMPRISON:
    {
        beam.range = LOS_MAX_RANGE;
        direction_chooser_args args;
        args.restricts = DIR_TARGET;
        args.mode = TARG_HOSTILE;
        args.needs_path = false;
        if (!spell_direction(*target, beam, &args))
            return spret::abort;

        if (beam.target == you.pos())
        {
            mpr("You cannot imprison yourself!");
            return spret::abort;
        }

        monster* mons = monster_at(beam.target);

        if (mons == nullptr || !you.can_see(*mons))
        {
            mpr("There is no monster there to imprison!");
            return spret::abort;
        }

        if (mons_is_firewood(*mons) || mons_is_conjured(mons->type))
        {
            mpr("You cannot imprison that!");
            return spret::abort;
        }

        if (mons->friendly() || mons->good_neutral())
        {
            mpr("You cannot imprison a law-abiding creature!");
            return spret::abort;
        }

        fail_check();

        int power = 3 + (roll_dice(5, you.skill(SK_INVOCATIONS, 5) + 12) / 26);

        if (!cast_imprison(power, mons, -GOD_ZIN))
            return spret::abort;
        break;
    }

    case ABIL_ZIN_SANCTUARY:
        fail_check();
        zin_sanctuary();
        break;

    case ABIL_ZIN_DONATE_GOLD:
        fail_check();
        zin_donate_gold();
        break;

    case ABIL_TSO_DIVINE_SHIELD:
        fail_check();
        tso_divine_shield();
        break;

    case ABIL_TSO_CLEANSING_FLAME:
    {
        targeter_radius hitfunc(&you, LOS_SOLID, 2);
        {
            if (stop_attack_prompt(hitfunc, "harm", _cleansing_flame_affects))
                return spret::abort;
        }
        fail_check();
        cleansing_flame(10 + you.skill_rdiv(SK_INVOCATIONS, 7, 6),
                        cleansing_flame_source::invocation, you.pos(), &you);
        break;
    }

    case ABIL_TSO_SUMMON_DIVINE_WARRIOR:
        fail_check();
        summon_holy_warrior(you.skill(SK_INVOCATIONS, 4), false);
        break;

    case ABIL_TSO_BLESS_WEAPON:
        fail_check();
        simple_god_message(" will bless one of your weapons.");
        // included in default force_more_message
        if (!bless_weapon(GOD_SHINING_ONE, SPWPN_HOLY_WRATH, YELLOW))
            return spret::abort;
        break;

    case ABIL_KIKU_RECEIVE_CORPSES:
        fail_check();
        kiku_receive_corpses(you.skill(SK_NECROMANCY, 4));
        break;

    case ABIL_KIKU_TORMENT:
        fail_check();
        if (!kiku_take_corpse()) // Should always succeed.
        {
            mpr("There are no nearby corpses to sacrifice!");
            return spret::success;
        }
        simple_god_message(" torments the living!");
        torment(&you, TORMENT_KIKUBAAQUDGHA, you.pos());
        break;

    case ABIL_KIKU_BLESS_WEAPON:
        fail_check();
        simple_god_message(" will bloody one of your weapons with pain.");
        // included in default force_more_message
        if (!bless_weapon(GOD_KIKUBAAQUDGHA, SPWPN_PAIN, RED))
            return spret::abort;
        break;

    case ABIL_KIKU_GIFT_CAPSTONE_SPELLS:
    {
        fail_check();
        if (!kiku_gift_capstone_spells())
            return spret::abort;
        break;
    }

    case ABIL_YRED_INJURY_MIRROR:
        fail_check();
        if (yred_injury_mirror())
            mpr("Another wave of unholy energy enters you.");
        else
        {
            mprf("You offer yourself to %s, and are filled with unholy energy.",
                 god_name(you.religion).c_str());
        }
        you.duration[DUR_MIRROR_DAMAGE] = 9 * BASELINE_DELAY
                     + random2avg(you.piety * BASELINE_DELAY, 2) / 10;
        break;

    case ABIL_YRED_ANIMATE_REMAINS:
        fail_check();
        canned_msg(MSG_ANIMATE_REMAINS);
        if (animate_remains(you.pos(), CORPSE_BODY, BEH_FRIENDLY, 0,
                            MHITYOU, &you, "", GOD_YREDELEMNUL) < 0)
        {
            mpr("There are no remains here to animate!");
            return spret::abort;
        }
        break;

    case ABIL_YRED_ANIMATE_DEAD:
        fail_check();
        canned_msg(MSG_CALL_DEAD);
        if (!animate_dead(&you, you.skill_rdiv(SK_INVOCATIONS) + 1,
                         BEH_FRIENDLY, MHITYOU, &you, "", GOD_YREDELEMNUL))
        {
            mpr("There are no remains here to animate!");
            return spret::abort;
        }

        break;

    case ABIL_YRED_RECALL_UNDEAD_SLAVES:
        fail_check();
        start_recall(recall_t::yred);
        break;

    case ABIL_YRED_DRAIN_LIFE:
    {
        int damage = 0;
        const int pow = you.skill_rdiv(SK_INVOCATIONS);

        if (trace_los_attack_spell(SPELL_DRAIN_LIFE, pow, &you) == spret::abort
            && !yesno("There are no drainable targets visible. Drain Life "
                      "anyway?", true, 'n'))
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }

        const spret result = fire_los_attack_spell(SPELL_DRAIN_LIFE, pow,
                                                   &you, fail, &damage);
        if (result != spret::success)
            return result;

        if (damage > 0)
        {
            mpr("You feel life flooding into your body.");
            inc_hp(damage);
        }
        break;
    }

    case ABIL_YRED_ENSLAVE_SOUL:
    {
        god_acting gdact;
        beam.range = LOS_MAX_RANGE;
        direction_chooser_args args;
        args.restricts = DIR_TARGET;
        args.mode = TARG_HOSTILE;
        args.needs_path = false;

        if (!spell_direction(*target, beam, &args))
            return spret::abort;

        if (beam.target == you.pos())
        {
            mpr("Your soul already belongs to Yredelemnul.");
            return spret::abort;
        }

        monster* mons = monster_at(beam.target);

        if (mons && you.can_see(*mons) && mons->is_illusion())
        {
            fail_check();
            simple_monster_message(*mons, "'s clone doesn't have a soul to enslave!");
            // Still costs a turn to gain the information.
            return spret::success;
        }

        if (mons == nullptr || !you.can_see(*mons)
            || !yred_can_enslave_soul(mons))
        {
            mpr("You see nothing there you can enslave the soul of!");
            return spret::abort;
        }

        // The monster can be no more than lightly wounded/damaged.
        if (mons_get_damage_level(*mons) > MDAM_LIGHTLY_DAMAGED)
        {
            simple_monster_message(*mons, "'s soul is too badly injured.");
            return spret::abort;
        }
        fail_check();

        const int duration = you.skill_rdiv(SK_INVOCATIONS, 3, 4) + 2;
        mons->add_ench(mon_enchant(ENCH_SOUL_RIPE, 0, &you,
                                   duration * BASELINE_DELAY));
        simple_monster_message(*mons, "'s soul is now ripe for the taking.");
        break;
    }

    case ABIL_OKAWARU_HEROISM:
        fail_check();
        mprf(MSGCH_DURATION, you.duration[DUR_HEROISM]
             ? "You feel more confident with your borrowed prowess."
             : "You gain the combat prowess of a mighty hero.");

        you.increase_duration(DUR_HEROISM,
                              10 + random2avg(you.skill(SK_INVOCATIONS, 6), 2),
                              100);
        you.redraw_evasion      = true;
        you.redraw_armour_class = true;
        break;

    case ABIL_OKAWARU_FINESSE:
        fail_check();
        if (you.duration[DUR_FINESSE])
        {
            // "Your [hand(s)] get{s} new energy."
            mprf(MSGCH_DURATION, "%s",
                 you.hands_act("get", "new energy.").c_str());
        }
        else
            mprf(MSGCH_DURATION, "You can now deal lightning-fast blows.");

        you.increase_duration(DUR_FINESSE,
                              10 + random2avg(you.skill(SK_INVOCATIONS, 6), 2),
                              100);

        did_god_conduct(DID_HASTY, 8); // Currently irrelevant.
        break;

    case ABIL_OKAWARU_DUEL:
        return okawaru_duel(fail);

    case ABIL_MAKHLEB_MINOR_DESTRUCTION:
    {
        // TODO: range check duplicated for UI/messaging purposes in quiver.cc,
        // _ability_quiver_range_check
        beam.range = min((int)you.current_vision, 5);

        if (!spell_direction(*target, beam))
            return spret::abort;

        int power = you.skill(SK_INVOCATIONS, 1)
                    + random2(1 + you.skill(SK_INVOCATIONS, 1))
                    + random2(1 + you.skill(SK_INVOCATIONS, 1));

        // Since the actual beam is random, check with BEAM_MMISSILE.
        if (!player_tracer(ZAP_DEBUGGING_RAY, power, beam, beam.range))
            return spret::abort;

        fail_check();
        beam.origin_spell = SPELL_NO_SPELL; // let zapping reset this

        switch (random2(5))
        {
        case 0: zapping(ZAP_THROW_FLAME, power, beam); break;
        case 1: zapping(ZAP_PAIN, power, beam); break;
        case 2: zapping(ZAP_STONE_ARROW, power, beam); break;
        case 3: zapping(ZAP_SHOCK, power, beam); break;
        case 4: zapping(ZAP_BREATHE_ACID, power / 7, beam); break;
        }
        break;
    }

    case ABIL_MAKHLEB_LESSER_SERVANT_OF_MAKHLEB:
        summon_demon_type(random_choose(MONS_HELLWING, MONS_NEQOXEC,
                                        MONS_ORANGE_DEMON, MONS_SMOKE_DEMON,
                                        MONS_YNOXINUL),
                          20 + you.skill(SK_INVOCATIONS, 3),
                          GOD_MAKHLEB, 0, !fail);
        break;

    case ABIL_MAKHLEB_MAJOR_DESTRUCTION:
    {
        beam.range = you.current_vision;

        if (!spell_direction(*target, beam))
            return spret::abort;

        int power = you.skill(SK_INVOCATIONS, 2)
                    + random2(1 + you.skill(SK_INVOCATIONS, 2))
                    + random2(1 + you.skill(SK_INVOCATIONS, 2));

        // Since the actual beam is random, check with BEAM_MMISSILE.
        if (!player_tracer(ZAP_DEBUGGING_RAY, power, beam, beam.range))
            return spret::abort;

        fail_check();
        {
            beam.origin_spell = SPELL_NO_SPELL; // let zapping reset this
            zap_type ztype =
                random_choose(ZAP_BOLT_OF_FIRE,
                              ZAP_LIGHTNING_BOLT,
                              ZAP_BOLT_OF_MAGMA,
                              ZAP_BOLT_OF_DRAINING,
                              ZAP_CORROSIVE_BOLT);
            zapping(ztype, power, beam);
        }
        break;
    }

    case ABIL_MAKHLEB_GREATER_SERVANT_OF_MAKHLEB:
        summon_demon_type(random_choose(MONS_EXECUTIONER, MONS_GREEN_DEATH,
                                        MONS_BLIZZARD_DEMON, MONS_BALRUG,
                                        MONS_CACODEMON),
                          20 + you.skill(SK_INVOCATIONS, 3),
                          GOD_MAKHLEB, 0, !fail);
        break;

    case ABIL_TROG_BERSERK:
        fail_check();
        // Trog abilities don't use or train invocations.
        you.go_berserk(true);
        break;

    case ABIL_TROG_HAND:
        fail_check();
        // Trog abilities don't use or train invocations.
        trog_do_trogs_hand(you.piety / 2);
        break;

    case ABIL_TROG_BROTHERS_IN_ARMS:
        fail_check();
        // Trog abilities don't use or train invocations.
        summon_berserker(you.piety +
                         random2(you.piety/4) - random2(you.piety/4),
                         &you);
        break;

    case ABIL_SIF_MUNA_FORGET_SPELL:
        fail_check();
        if (cast_selective_amnesia() <= 0)
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_SIF_MUNA_CHANNEL_ENERGY:
    {
        fail_check();
        you.increase_duration(DUR_CHANNEL_ENERGY,
            4 + random2avg(you.skill_rdiv(SK_INVOCATIONS, 2, 3), 2), 100);
        break;
    }

    case ABIL_SIF_MUNA_DIVINE_EXEGESIS:
        return divine_exegesis(fail);

    case ABIL_ELYVILON_HEAL_SELF:
    {
        fail_check();
        const int pow = min(50, 10 + you.skill_rdiv(SK_INVOCATIONS, 1, 3));
        const int healed = pow + roll_dice(2, pow) - 2;
        mpr("You are healed.");
        inc_hp(healed);
        break;
    }

    case ABIL_ELYVILON_PURIFICATION:
        fail_check();
        elyvilon_purification();
        break;

    case ABIL_ELYVILON_HEAL_OTHER:
    {
        int pow = 30 + you.skill(SK_INVOCATIONS, 1);
        return cast_healing(pow, fail);
    }

    case ABIL_ELYVILON_DIVINE_VIGOUR:
        fail_check();
        if (!elyvilon_divine_vigour())
            return spret::abort;
        break;

    case ABIL_LUGONU_ABYSS_EXIT:
        if (cancel_harmful_move(false))
            return spret::abort;
        fail_check();
        down_stairs(DNGN_EXIT_ABYSS);
        break;

    case ABIL_LUGONU_BEND_SPACE:
        if (cancel_harmful_move(false))
            return spret::abort;
        fail_check();
        lugonu_bend_space();
        break;

    case ABIL_LUGONU_BANISH:
    {
        beam.range = you.current_vision;
        const int pow = 68 + you.skill(SK_INVOCATIONS, 3);

        direction_chooser_args args;
        args.mode = TARG_HOSTILE;
        args.get_desc_func = bind(desc_wl_success_chance, placeholders::_1,
                                  zap_ench_power(ZAP_BANISHMENT, pow, false),
                                  nullptr);
        if (!spell_direction(*target, beam, &args))
            return spret::abort;

        if (beam.target == you.pos())
        {
            mpr("You cannot banish yourself!");
            return spret::abort;
        }

        fail_check();

        return zapping(ZAP_BANISHMENT, pow, beam, true, nullptr, fail);
    }

    case ABIL_LUGONU_CORRUPT:
        fail_check();
        if (!lugonu_corrupt_level(300 + you.skill(SK_INVOCATIONS, 15)))
            return spret::abort;
        break;

    case ABIL_LUGONU_ABYSS_ENTER:
    {
        if (cancel_harmful_move(false))
            return spret::abort;
        fail_check();
        // Deflate HP.
        dec_hp(random2avg(you.hp, 2), false);

        no_notes nx; // This banishment shouldn't be noted.
        banished();
        break;
    }

    case ABIL_LUGONU_BLESS_WEAPON:
        fail_check();
        simple_god_message(" will brand one of your weapons with the "
                           "corruption of the Abyss.");
        // included in default force_more_message
        if (!bless_weapon(GOD_LUGONU, SPWPN_DISTORTION, MAGENTA))
            return spret::abort;
        break;

    case ABIL_NEMELEX_DRAW_DESTRUCTION:
        fail_check();
        if (!deck_draw(DECK_OF_DESTRUCTION))
            return spret::abort;
        break;
    case ABIL_NEMELEX_DRAW_ESCAPE:
        fail_check();
        if (!deck_draw(DECK_OF_ESCAPE))
            return spret::abort;
        break;
    case ABIL_NEMELEX_DRAW_SUMMONING:
        fail_check();
        if (!deck_draw(DECK_OF_SUMMONING))
            return spret::abort;
        break;
    case ABIL_NEMELEX_DRAW_STACK:
        fail_check();
        if (!deck_draw(DECK_STACK))
            return spret::abort;
        break;

    case ABIL_NEMELEX_TRIPLE_DRAW:
        fail_check();
        if (!deck_triple_draw())
            return spret::abort;
        break;

    case ABIL_NEMELEX_DEAL_FOUR:
        fail_check();
        if (!deck_deal())
            return spret::abort;
        break;

    case ABIL_NEMELEX_STACK_FIVE:
        fail_check();
        if (!deck_stack())
            return spret::abort;
        break;

    case ABIL_BEOGH_SMITING:
        fail_check();
        if (your_spells(SPELL_SMITING,
                        12 + skill_bump(SK_INVOCATIONS, 6),
                        false, nullptr, target) == spret::abort)
        {
            return spret::abort;
        }
        break;

    case ABIL_BEOGH_GIFT_ITEM:
        if (!beogh_gift_item())
            return spret::abort;
        break;

    case ABIL_BEOGH_RESURRECTION:
        if (!beogh_resurrect())
            return spret::abort;
        break;

    case ABIL_BEOGH_RECALL_ORCISH_FOLLOWERS:
        fail_check();
        start_recall(recall_t::beogh);
        break;

    case ABIL_STOP_RECALL:
        fail_check();
        mpr("You stop recalling your allies.");
        end_recall();
        break;

    case ABIL_FEDHAS_WALL_OF_BRIARS:
        fail_check();
        fedhas_wall_of_briars();
        break;

    case ABIL_FEDHAS_GROW_BALLISTOMYCETE:
        return fedhas_grow_ballistomycete(fail);

    case ABIL_FEDHAS_OVERGROW:
        return fedhas_overgrow(fail);

    case ABIL_FEDHAS_GROW_OKLOB:
        return fedhas_grow_oklob(fail);

    case ABIL_TRAN_BAT:
    {
        if (_player_cancels_vampire_bat_transformation())
            return spret::abort;
        fail_check();
        if (!transform(100, transformation::bat))
        {
            crawl_state.zero_turns_taken();
            return spret::abort;
        }

        _cause_vampire_bat_form_stat_drain();

        break;
    }

    case ABIL_EXSANGUINATE:
        fail_check();
        start_delay<ExsanguinateDelay>(5);
        break;

    case ABIL_REVIVIFY:
        fail_check();
        start_delay<RevivifyDelay>(5);
        break;

    case ABIL_JIYVA_SLIMIFY:
    {
        fail_check();
        const item_def* const weapon = you.weapon();
        const string msg = weapon ? weapon->name(DESC_YOUR)
                                  : ("your " + you.hand_name(true));
        mprf(MSGCH_DURATION, "A thick mucus forms on %s.", msg.c_str());
        you.increase_duration(DUR_SLIMIFY,
                              random2avg(you.piety / 4, 2) + 3, 100);
        break;
    }

    case ABIL_JIYVA_OOZEMANCY:
        return jiyva_oozemancy(fail);

    case ABIL_CHEIBRIADOS_TIME_STEP:
        fail_check();
        cheibriados_time_step(max(1, you.skill(SK_INVOCATIONS, 10)
                                     * you.piety / 100));
        break;

    case ABIL_CHEIBRIADOS_TIME_BEND:
        fail_check();
        cheibriados_time_bend(16 + you.skill(SK_INVOCATIONS, 8));
        break;

    case ABIL_CHEIBRIADOS_DISTORTION:
        fail_check();
        cheibriados_temporal_distortion();
        break;

    case ABIL_CHEIBRIADOS_SLOUCH:
        fail_check();
        if (!cheibriados_slouch())
            return spret::abort;
        break;

    case ABIL_ASHENZARI_CURSE:
    {
        fail_check();
        if (!ashenzari_curse_item())
            return spret::abort;
        break;
    }

    case ABIL_ASHENZARI_UNCURSE:
        fail_check();
        if (!ashenzari_uncurse_item())
            return spret::abort;
        break;

    case ABIL_DITHMENOS_SHADOW_STEP:
        if (_abort_if_stationary() || cancel_harmful_move(false))
            return spret::abort;
        fail_check();
        if (!dithmenos_shadow_step()) // TODO dist arg
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_DITHMENOS_SHADOW_FORM:
        fail_check();
        if (!transform(you.skill(SK_INVOCATIONS, 2), transformation::shadow))
        {
            crawl_state.zero_turns_taken();
            return spret::abort;
        }
        break;

    case ABIL_GOZAG_POTION_PETITION:
        fail_check();
        run_uncancel(UNC_POTION_PETITION, 0);
        break;

    case ABIL_GOZAG_CALL_MERCHANT:
        fail_check();
        run_uncancel(UNC_CALL_MERCHANT, 0);
        break;

    case ABIL_GOZAG_BRIBE_BRANCH:
        fail_check();
        if (!gozag_bribe_branch())
            return spret::abort;
        break;

    case ABIL_QAZLAL_UPHEAVAL:
        return qazlal_upheaval(coord_def(), false, fail, target);

    case ABIL_QAZLAL_ELEMENTAL_FORCE:
        return qazlal_elemental_force(fail);

    case ABIL_QAZLAL_DISASTER_AREA:
        fail_check();
        if (!qazlal_disaster_area())
            return spret::abort;
        break;

    case ABIL_RU_SACRIFICE_PURITY:
    case ABIL_RU_SACRIFICE_WORDS:
    case ABIL_RU_SACRIFICE_DRINK:
    case ABIL_RU_SACRIFICE_ESSENCE:
    case ABIL_RU_SACRIFICE_HEALTH:
    case ABIL_RU_SACRIFICE_STEALTH:
    case ABIL_RU_SACRIFICE_ARTIFICE:
    case ABIL_RU_SACRIFICE_LOVE:
    case ABIL_RU_SACRIFICE_COURAGE:
    case ABIL_RU_SACRIFICE_ARCANA:
    case ABIL_RU_SACRIFICE_NIMBLENESS:
    case ABIL_RU_SACRIFICE_DURABILITY:
    case ABIL_RU_SACRIFICE_HAND:
    case ABIL_RU_SACRIFICE_EXPERIENCE:
    case ABIL_RU_SACRIFICE_SKILL:
    case ABIL_RU_SACRIFICE_EYE:
    case ABIL_RU_SACRIFICE_RESISTANCE:
        fail_check();
        if (!ru_do_sacrifice(abil.ability))
            return spret::abort;
        break;

    case ABIL_RU_REJECT_SACRIFICES:
        fail_check();
        if (!ru_reject_sacrifices())
            return spret::abort;
        break;

    case ABIL_RU_DRAW_OUT_POWER:
        fail_check();
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You're too exhausted to draw out your power.");
            return spret::abort;
        }
        if (you.hp == you.hp_max && you.magic_points == you.max_magic_points
            && !you.duration[DUR_CONF]
            && !you.duration[DUR_SLOW]
            && !you.attribute[ATTR_HELD]
            && !you.petrifying()
            && !you.is_constricted())
        {
            mpr("You have no need to draw out power.");
            return spret::abort;
        }
        ru_draw_out_power();
        you.increase_duration(DUR_EXHAUSTED, 12 + random2(5));
        break;

    case ABIL_RU_POWER_LEAP:
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You're too exhausted to power leap.");
            return spret::abort;
        }

        if (_abort_if_stationary() || cancel_harmful_move())
            return spret::abort;

        fail_check();

        if (!ru_power_leap()) // TODO dist arg
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        you.increase_duration(DUR_EXHAUSTED, 18 + random2(8));
        break;

    case ABIL_RU_APOCALYPSE:
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You're too exhausted to unleash your apocalyptic power.");
            return spret::abort;
        }

        fail_check();

        if (!ru_apocalypse())
            return spret::abort;
        you.increase_duration(DUR_EXHAUSTED, 30 + random2(20));
        break;

    case ABIL_USKAYAW_STOMP:
        fail_check();
        if (!uskayaw_stomp())
            return spret::abort;
        break;

    case ABIL_USKAYAW_LINE_PASS:
        if (_abort_if_stationary() || cancel_harmful_move())
            return spret::abort;
        fail_check();
        if (!uskayaw_line_pass()) // TODO dist arg
            return spret::abort;
        break;

    case ABIL_USKAYAW_GRAND_FINALE:
        if (cancel_harmful_move(false))
            return spret::abort;
        return uskayaw_grand_finale(fail); // TODO dist arg

    case ABIL_HEPLIAKLQANA_IDEALISE:
        return hepliaklqana_idealise(fail);

    case ABIL_HEPLIAKLQANA_RECALL:
        fail_check();
        if (try_recall(hepliaklqana_ancestor()))
            upgrade_hepliaklqana_ancestor(true);
        break;

    case ABIL_HEPLIAKLQANA_TRANSFERENCE:
        return hepliaklqana_transference(fail); // TODO: dist arg

    case ABIL_HEPLIAKLQANA_TYPE_KNIGHT:
    case ABIL_HEPLIAKLQANA_TYPE_BATTLEMAGE:
    case ABIL_HEPLIAKLQANA_TYPE_HEXER:
        if (!hepliaklqana_choose_ancestor_type(abil.ability))
            return spret::abort;
        break;

    case ABIL_HEPLIAKLQANA_IDENTITY:
        hepliaklqana_choose_identity();
        break;

    case ABIL_WU_JIAN_SERPENTS_LASH:
        if (you.attribute[ATTR_SERPENTS_LASH])
        {
            mpr("You are already lashing out.");
            return spret::abort;
        }
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You are too exhausted to lash out.");
            return spret::abort;
        }
        fail_check();
        mprf(MSGCH_GOD, "Your muscles tense, ready for explosive movement...");
        you.attribute[ATTR_SERPENTS_LASH] = 2;
        you.redraw_status_lights = true;
        return spret::success;

    case ABIL_WU_JIAN_HEAVENLY_STORM:
        if (you.props.exists(WU_JIAN_HEAVENLY_STORM_KEY))
        {
            mpr("You are already engulfed in a heavenly storm!");
            return spret::abort;
        }
        fail_check();
        wu_jian_heavenly_storm();
        break;

    case ABIL_WU_JIAN_WALLJUMP:
        fail_check();
        return wu_jian_wall_jump_ability();

    case ABIL_IGNIS_FIERY_ARMOUR:
        fail_check();
        fiery_armour();
        return spret::success;

    case ABIL_IGNIS_FOXFIRE:
        return foxfire_swarm();

    case ABIL_IGNIS_RISING_FLAME:
        if (!_can_rising_flame(false))
            return spret::abort;
        mpr("You begin to rise into the air.");
        // slightly faster than teleport
        you.set_duration(DUR_RISING_FLAME, 2 + random2(3));
        you.one_time_ability_used.set(GOD_IGNIS);
        return spret::success;

    case ABIL_RENOUNCE_RELIGION:
        fail_check();
        if (yesno("Really renounce your faith, foregoing its fabulous benefits?",
                  false, 'n')
            && yesno("Are you sure?", false, 'n'))
        {
            excommunication(true);
        }
        else
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_CONVERT_TO_BEOGH:
        fail_check();
        god_pitch(GOD_BEOGH);
        if (you_worship(GOD_BEOGH))
        {
            spare_beogh_convert();
            break;
        }
        return spret::abort;

#ifdef WIZARD
    case ABIL_WIZ_BUILD_TERRAIN:
    {
        int &last_feat = you.props[WIZ_LAST_FEATURE_TYPE_PROP].get_int();
        if (!wizard_create_feature(*target,
                        static_cast<dungeon_feature_type>(last_feat), false))
        {
            return spret::abort;
        }
        break;
    }
    case ABIL_WIZ_CLEAR_TERRAIN:
        if (!wizard_create_feature(*target, DNGN_FLOOR, false))
            return spret::abort;
        break;
    case ABIL_WIZ_SET_TERRAIN:
    {
        auto feat = wizard_select_feature(false);
        if (feat == DNGN_UNSEEN)
            return spret::abort;
        you.props[WIZ_LAST_FEATURE_TYPE_PROP] = static_cast<int>(feat);
        mprf("Now building '%s'", dungeon_feature_name(feat));
        break;
    }
#endif
    case ABIL_NON_ABILITY:
        fail_check();
        mpr("Sorry, you can't do that.");
        break;

    default:
        die("invalid ability");
    }

    return spret::success;
}

// [ds] Increase piety cost for god abilities that are particularly
// overpowered in Sprint. Yes, this is a hack. No, I don't care.
static int _scale_piety_cost(ability_type abil, int original_cost)
{
    // Abilities that have aroused our ire earn 2.5x their classic
    // Crawl piety cost.
    return (crawl_state.game_is_sprint()
            && (abil == ABIL_TROG_BROTHERS_IN_ARMS
                || abil == ABIL_MAKHLEB_GREATER_SERVANT_OF_MAKHLEB))
           ? div_rand_round(original_cost * 5, 2)
           : original_cost;
}

static void _pay_ability_costs(const ability_def& abil)
{
    // wall jump handles its own timing, because it can be instant if
    // serpent's lash is activated.
    if (abil.flags & abflag::instant)
    {
        you.turn_is_over = false;
        you.elapsed_time_at_last_input = you.elapsed_time;
        update_turn_count();
    }
    else if (abil.ability != ABIL_WU_JIAN_WALLJUMP)
        you.turn_is_over = true;

    const int piety_cost =
        _scale_piety_cost(abil.ability, abil.piety_cost.cost());
    const int hp_cost    = abil.get_hp_cost();
    const int mp_cost = abil.get_mp_cost();

    dprf("Cost: mp=%d; hp=%d; piety=%d",
         mp_cost, hp_cost, piety_cost);

    if (mp_cost)
    {
        pay_mp(mp_cost);
        finalize_mp_cost();
    }

    if (hp_cost)
        dec_hp(hp_cost, false);

    if (piety_cost)
        lose_piety(piety_cost);
}

int choose_ability_menu(const vector<talent>& talents)
{
    ToggleableMenu abil_menu(MF_SINGLESELECT | MF_ANYPRINTABLE
            | MF_NO_WRAP_ROWS | MF_TOGGLE_ACTION | MF_ALWAYS_SHOW_MORE);

    abil_menu.set_highlighter(nullptr);
#ifdef USE_TILE_LOCAL
    {
        // Hack like the one in spl-cast.cc:list_spells() to align the title.
        ToggleableMenuEntry* me =
            new ToggleableMenuEntry("Ability - do what?                  "
                                    "Cost                            Failure",
                                    "Ability - describe what?            "
                                    "Cost                            Failure",
                                    MEL_ITEM);
        me->colour = BLUE;
        abil_menu.set_title(me, true, true);
    }
#else
    abil_menu.set_title(
        new ToggleableMenuEntry("Ability - do what?                  "
                                "Cost                            Failure",
                                "Ability - describe what?            "
                                "Cost                            Failure",
                                MEL_TITLE), true, true);
#endif
    abil_menu.set_tag("ability");
    abil_menu.add_toggle_key('!');
    abil_menu.add_toggle_key('?');
    abil_menu.menu_action = Menu::ACT_EXECUTE;

    if (crawl_state.game_is_hints())
    {
        // XXX: This could be buggy if you manage to pick up lots and
        // lots of abilities during hints mode.
        abil_menu.set_more(hints_abilities_info());
    }
    else
    {
        abil_menu.set_more(formatted_string::parse_string(
                           "Press '<w>!</w>' or '<w>?</w>' to toggle "
                           "between ability selection and description."));
    }

    int numbers[52];
    for (int i = 0; i < 52; ++i)
        numbers[i] = i;

    bool found_invocations = false;

    // First add all non-invocation abilities.
    for (unsigned int i = 0; i < talents.size(); ++i)
    {
        if (talents[i].is_invocation)
            found_invocations = true;
        else
        {
            ToggleableMenuEntry* me =
                new ToggleableMenuEntry(describe_talent(talents[i]),
                                        describe_talent(talents[i]),
                                        MEL_ITEM, 1, talents[i].hotkey);
            me->data = &numbers[i];
            me->add_tile(tile_def(tileidx_ability(talents[i].which)));
            if (!check_ability_possible(talents[i].which, true))
            {
                me->colour = COL_INAPPLICABLE;
                me->add_tile(tile_def(TILEI_MESH));
            }
            else if (_check_ability_dangerous(talents[i].which, true))
                me->colour = COL_DANGEROUS;
            abil_menu.add_entry(me);
        }
    }

    if (found_invocations)
    {
#ifdef USE_TILE_LOCAL
        MenuEntry* subtitle = new MenuEntry(" Invocations -    ", MEL_ITEM);
        subtitle->colour = BLUE;
        abil_menu.add_entry(subtitle);
#else
        abil_menu.add_entry(new MenuEntry(" Invocations -    ", MEL_SUBTITLE));
#endif
        for (unsigned int i = 0; i < talents.size(); ++i)
        {
            if (talents[i].is_invocation)
            {
                ToggleableMenuEntry* me =
                    new ToggleableMenuEntry(describe_talent(talents[i]),
                                            describe_talent(talents[i]),
                                            MEL_ITEM, 1, talents[i].hotkey);
                me->data = &numbers[i];
                me->add_tile(tile_def(tileidx_ability(talents[i].which)));
                if (!check_ability_possible(talents[i].which, true))
                {
                    me->colour = COL_INAPPLICABLE;
                    me->add_tile(tile_def(TILEI_MESH));
                }
                else if (_check_ability_dangerous(talents[i].which, true))
                    me->colour = COL_DANGEROUS;
                abil_menu.add_entry(me);
            }
        }
    }

    int ret = -1;
    abil_menu.on_single_selection = [&abil_menu, &talents, &ret](const MenuEntry& sel)
    {
        ASSERT(sel.hotkeys.size() == 1);
        int selected = *(static_cast<int*>(sel.data));

        if (abil_menu.menu_action == Menu::ACT_EXAMINE)
            _print_talent_description(talents[selected]);
        else
            ret = *(static_cast<int*>(sel.data));
        return abil_menu.menu_action == Menu::ACT_EXAMINE;
    };
    abil_menu.show(false);
    if (!crawl_state.doing_prev_cmd_again)
    {
        redraw_screen();
        update_screen();
    }
    return ret;
}


string describe_talent(const talent& tal)
{
    ASSERT(tal.which != ABIL_NON_ABILITY);

    const string failure = failure_rate_to_string(tal.fail)
        + (testbits(get_ability_def(tal.which).flags, abflag::hostile)
           ? " hostile" : "");

    ostringstream desc;
    desc << left
         << chop_string(ability_name(tal.which), 32)
         << chop_string(make_cost_description(tal.which), 32)
         << chop_string(failure, 12);
    return trimmed_string(desc.str());
}

static void _add_talent(vector<talent>& vec, const ability_type ability,
                        bool check_confused)
{
    const talent t = get_talent(ability, check_confused);
    if (t.which != ABIL_NON_ABILITY)
        vec.push_back(t);
}

bool is_religious_ability(ability_type abil)
{
    // ignores abandon religion / convert to beogh
    return abil >= ABIL_FIRST_RELIGIOUS_ABILITY
        && abil <= ABIL_LAST_RELIGIOUS_ABILITY;
}

bool is_card_ability(ability_type abil)
{
    return testbits(get_ability_def(abil).flags, abflag::card);
}

bool player_has_ability(ability_type abil, bool include_unusable)
{
#ifdef WIZARD
    if (abil >= ABIL_FIRST_WIZ)
        return you.wizard;
#endif

    // TODO: consolidate fixup checks into here?
    abil = fixup_ability(abil);
    if (abil == ABIL_NON_ABILITY || abil == NUM_ABILITIES)
        return false;

    if (is_religious_ability(abil))
    {
        // TODO: something less dumb than this?
        auto god_abils = get_god_abilities(include_unusable, false,
                                               include_unusable);
        return count(god_abils.begin(), god_abils.end(), abil);
    }

    if (species::is_draconian(you.species)
        && species::draconian_breath(you.species) == abil)
    {
        return !form_changed_physiology() || you.form == transformation::dragon;
    }

    switch (abil)
    {
    case ABIL_HEAL_WOUNDS:
        return you.species == SP_DEEP_DWARF;
    case ABIL_SHAFT_SELF:
        if (crawl_state.game_is_sprint() || brdepth[you.where_are_you] == 1)
            return false;
        // fallthrough
    case ABIL_DIG:
        return you.can_burrow()
               && (form_keeps_mutations() || include_unusable);
    case ABIL_HOP:
        return you.get_mutation_level(MUT_HOP);
    case ABIL_ROLLING_CHARGE:
        return you.get_mutation_level(MUT_ROLL);
    case ABIL_BREATHE_POISON:
        return you.get_mutation_level(MUT_SPIT_POISON) >= 2;
    case ABIL_SPIT_POISON:
        return you.get_mutation_level(MUT_SPIT_POISON) == 1;
    case ABIL_REVIVIFY:
        return you.has_mutation(MUT_VAMPIRISM) && !you.vampire_alive;
    case ABIL_EXSANGUINATE:
        return you.has_mutation(MUT_VAMPIRISM) && you.vampire_alive;
    case ABIL_TRAN_BAT:
        return you.get_mutation_level(MUT_VAMPIRISM) >= 2
               && !you.vampire_alive
               && you.form != transformation::bat;
    case ABIL_BREATHE_FIRE:
        // red draconian handled before the switch
        return you.form == transformation::dragon
               && species::dragon_form(you.species) == MONS_FIRE_DRAGON;
    case ABIL_BLINKBOLT:
        return you.form == transformation::storm;
    // mutations
    case ABIL_DAMNATION:
        return you.get_mutation_level(MUT_HURL_DAMNATION);
    case ABIL_WORD_OF_CHAOS:
        return you.get_mutation_level(MUT_WORD_OF_CHAOS)
               && (!silenced(you.pos()) || include_unusable);
    case ABIL_END_TRANSFORMATION:
        return you.duration[DUR_TRANSFORMATION] && !you.transform_uncancellable;
    // TODO: other god abilities
    case ABIL_RENOUNCE_RELIGION:
        return !you_worship(GOD_NO_GOD);
    case ABIL_CONVERT_TO_BEOGH:
        return env.level_state & LSTATE_BEOGH && can_convert_to_beogh();
    // pseudo-evocations from equipped items
    case ABIL_EVOKE_BLINK:
        return you.scan_artefacts(ARTP_BLINK)
               && !you.get_mutation_level(MUT_NO_ARTIFICE);
    case ABIL_EVOKE_TURN_INVISIBLE:
        return you.evokable_invis()
               && !you.get_mutation_level(MUT_NO_ARTIFICE);
    case ABIL_EVOKE_ASMODEUS:
        return you.weapon()
               && is_unrandom_artefact(*you.weapon(), UNRAND_ASMODEUS);
    case ABIL_EVOKE_DISPATER:
        return you.weapon()
               && is_unrandom_artefact(*you.weapon(), UNRAND_DISPATER);
    case ABIL_EVOKE_OLGREB:
        return you.weapon()
               && is_unrandom_artefact(*you.weapon(), UNRAND_OLGREB);
    default:
        // removed abilities handled here
        return false;
    }
}

/**
 * Return all relevant talents that the player has.
 *
 * Currently the only abilities that are affected by include_unusable are god
 * abilities (affect by e.g. penance or silence).
 * @param check_confused If true, abilities that don't work when confused will
 *                       be excluded.
 * @param include_unusable If true, abilities that are currently unusable will
 *                         be excluded.
 * @return  A vector of talent structs.
 */
vector<talent> your_talents(bool check_confused, bool include_unusable, bool ignore_piety)
{
    vector<talent> talents;

    // TODO: can we just iterate over ability_type?
    vector<ability_type> check_order =
        { ABIL_HEAL_WOUNDS,
            ABIL_DIG,
            ABIL_SHAFT_SELF,
            ABIL_HOP,
            ABIL_ROLLING_CHARGE,
            ABIL_SPIT_POISON,
            ABIL_BREATHE_FIRE,
            ABIL_BREATHE_FROST,
            ABIL_BREATHE_POISON,
            ABIL_BREATHE_LIGHTNING,
            ABIL_BREATHE_POWER,
            ABIL_BREATHE_STEAM,
            ABIL_BREATHE_MEPHITIC,
            ABIL_BREATHE_ACID,
            ABIL_TRAN_BAT,
            ABIL_REVIVIFY,
            ABIL_EXSANGUINATE,
            ABIL_DAMNATION,
            ABIL_WORD_OF_CHAOS,
            ABIL_BLINKBOLT,
            ABIL_END_TRANSFORMATION,
            ABIL_RENOUNCE_RELIGION,
            ABIL_CONVERT_TO_BEOGH,
            ABIL_EVOKE_BLINK,
            ABIL_EVOKE_TURN_INVISIBLE,
            ABIL_EVOKE_ASMODEUS,
            ABIL_EVOKE_DISPATER,
            ABIL_EVOKE_OLGREB,
#ifdef WIZARD
            ABIL_WIZ_BUILD_TERRAIN,
            ABIL_WIZ_SET_TERRAIN,
            ABIL_WIZ_CLEAR_TERRAIN,
#endif
        };

    for (auto a : check_order)
        if (player_has_ability(a, include_unusable))
            _add_talent(talents, a, check_confused);


    // player_has_ability will just brute force these anyways (TODO)
    for (ability_type abil : get_god_abilities(include_unusable, ignore_piety,
                                               include_unusable))
    {
        _add_talent(talents, abil, check_confused);
    }

    // Side effect alert!
    // Find hotkeys for the non-hotkeyed talents.
    for (talent &tal : talents)
    {
        const int index = _lookup_ability_slot(tal.which);
        if (index > -1)
        {
            tal.hotkey = index_to_letter(index);
            continue;
        }

        // Try to find a free hotkey for i, starting from Z.
        for (int k = 51; k >= 0; --k)
        {
            const int kkey = index_to_letter(k);
            bool good_key = true;

            // Check that it doesn't conflict with other hotkeys.
            for (const talent &other : talents)
                if (other.hotkey == kkey)
                {
                    good_key = false;
                    break;
                }

            if (good_key)
            {
                tal.hotkey = kkey;
                you.ability_letter_table[k] = tal.which;
                break;
            }
        }
        // In theory, we could be left with an unreachable ability
        // here (if you have 53 or more abilities simultaneously).
    }

    return talents;
}

/**
 * Maybe move an ability to the slot given by the ability_slot option.
 *
 * @param[in] slot current slot of the ability
 * @returns the new slot of the ability; may still be slot, if the ability
 *          was not reassigned.
 */
int auto_assign_ability_slot(int slot)
{
    const ability_type abil_type = you.ability_letter_table[slot];
    const string abilname = lowercase_string(ability_name(abil_type));
    bool overwrite = false;
    // check to see whether we've chosen an automatic label:
    for (auto& mapping : Options.auto_ability_letters)
    {
        if (!mapping.first.matches(abilname))
            continue;
        for (char i : mapping.second)
        {
            if (i == '+')
                overwrite = true;
            else if (i == '-')
                overwrite = false;
            else if (isaalpha(i))
            {
                const int index = letter_to_index(i);
                ability_type existing_ability = you.ability_letter_table[index];

                if (existing_ability == ABIL_NON_ABILITY
                    || existing_ability == abil_type)
                {
                    // Unassigned or already assigned to this ability.
                    you.ability_letter_table[index] = abil_type;
                    if (slot != index)
                        you.ability_letter_table[slot] = ABIL_NON_ABILITY;
                    return index;
                }
                else if (overwrite)
                {
                    const string str = lowercase_string(ability_name(existing_ability));
                    // Don't overwrite an ability matched by the same rule.
                    if (mapping.first.matches(str))
                        continue;
                    you.ability_letter_table[slot] = abil_type;
                    swap_ability_slots(slot, index, true);
                    return index;
                }
                // else occupied, continue to the next mapping.
            }
        }
    }
    return slot;
}

// Returns an index (0-51) if already assigned, -1 if not.
static int _lookup_ability_slot(const ability_type abil)
{
    // Placeholder handling, part 2: The ability we have might
    // correspond to a placeholder, in which case the ability letter
    // table will contain that placeholder. Convert the latter to
    // its corresponding ability before comparing the two, so that
    // we'll find the placeholder's index properly.
    for (int slot = 0; slot < 52; slot++)
        if (fixup_ability(you.ability_letter_table[slot]) == abil)
            return slot;
    return -1;
}

// Assign a new ability slot if necessary. Returns an index (0-51) if
// successful, -1 if you should just use the next one.
int find_ability_slot(const ability_type abil, char firstletter)
{
    // If we were already assigned a slot, use it.
    int had_slot = _lookup_ability_slot(abil);
    if (had_slot > -1)
        return had_slot;

    // No requested slot, find new one and make it preferred.

    // firstletter defaults to 'f', because a-e is for invocations
    int first_slot = letter_to_index(firstletter);

    // Reserve the first non-god ability slot (f) for Draconian breath
    if (you.species == SP_BASE_DRACONIAN && first_slot >= letter_to_index('f'))
        first_slot += 1;

    ASSERT(first_slot < 52);

    switch (abil)
    {
    case ABIL_KIKU_GIFT_CAPSTONE_SPELLS:
        first_slot = letter_to_index('N');
        break;
    case ABIL_TSO_BLESS_WEAPON:
    case ABIL_KIKU_BLESS_WEAPON:
    case ABIL_LUGONU_BLESS_WEAPON:
        first_slot = letter_to_index('W');
        break;
    case ABIL_CONVERT_TO_BEOGH:
        first_slot = letter_to_index('Y');
        break;
    case ABIL_RU_SACRIFICE_PURITY:
    case ABIL_RU_SACRIFICE_WORDS:
    case ABIL_RU_SACRIFICE_DRINK:
    case ABIL_RU_SACRIFICE_ESSENCE:
    case ABIL_RU_SACRIFICE_HEALTH:
    case ABIL_RU_SACRIFICE_STEALTH:
    case ABIL_RU_SACRIFICE_ARTIFICE:
    case ABIL_RU_SACRIFICE_LOVE:
    case ABIL_RU_SACRIFICE_COURAGE:
    case ABIL_RU_SACRIFICE_ARCANA:
    case ABIL_RU_SACRIFICE_NIMBLENESS:
    case ABIL_RU_SACRIFICE_DURABILITY:
    case ABIL_RU_SACRIFICE_HAND:
    case ABIL_RU_SACRIFICE_EXPERIENCE:
    case ABIL_RU_SACRIFICE_SKILL:
    case ABIL_RU_SACRIFICE_EYE:
    case ABIL_RU_SACRIFICE_RESISTANCE:
    case ABIL_RU_REJECT_SACRIFICES:
    case ABIL_HEPLIAKLQANA_TYPE_KNIGHT:
    case ABIL_HEPLIAKLQANA_TYPE_BATTLEMAGE:
    case ABIL_HEPLIAKLQANA_TYPE_HEXER:
    case ABIL_HEPLIAKLQANA_IDENTITY: // move this?
    case ABIL_ASHENZARI_CURSE:
    case ABIL_ASHENZARI_UNCURSE:
        first_slot = letter_to_index('G');
        break;
#ifdef WIZARD
    case ABIL_WIZ_BUILD_TERRAIN:
    case ABIL_WIZ_SET_TERRAIN:
    case ABIL_WIZ_CLEAR_TERRAIN:
        first_slot = letter_to_index('O'); // somewhat arbitrary, late in the alphabet
#endif
    default:
        break;
    }

    for (int slot = first_slot; slot < 52; ++slot)
    {
        if (you.ability_letter_table[slot] == ABIL_NON_ABILITY)
        {
            you.ability_letter_table[slot] = abil;
            return auto_assign_ability_slot(slot);
        }
    }

    // If we can't find anything else, try a-e.
    for (int slot = first_slot - 1; slot >= 0; --slot)
    {
        if (you.ability_letter_table[slot] == ABIL_NON_ABILITY)
        {
            you.ability_letter_table[slot] = abil;
            return auto_assign_ability_slot(slot);
        }
    }

    // All letters are assigned.
    return -1;
}


vector<ability_type> get_god_abilities(bool ignore_silence, bool ignore_piety,
                                       bool ignore_penance)
{
    vector<ability_type> abilities;
    if (you_worship(GOD_RU) && you.props.exists(AVAILABLE_SAC_KEY))
    {
        bool any_sacrifices = false;
        for (const auto& store : you.props[AVAILABLE_SAC_KEY].get_vector())
        {
            any_sacrifices = true;
            abilities.push_back(static_cast<ability_type>(store.get_int()));
        }
        if (any_sacrifices)
            abilities.push_back(ABIL_RU_REJECT_SACRIFICES);
    }
    if (you_worship(GOD_ASHENZARI))
    {
        if (you.props.exists(AVAILABLE_CURSE_KEY))
            abilities.push_back(ABIL_ASHENZARI_CURSE);
        if (ignore_piety || you.piety > ASHENZARI_BASE_PIETY )
            abilities.push_back(ABIL_ASHENZARI_UNCURSE);
    }
    // XXX: should we check ignore_piety?
    if (you_worship(GOD_HEPLIAKLQANA)
        && piety_rank() >= 2 && !you.props.exists(HEPLIAKLQANA_ALLY_TYPE_KEY))
    {
        for (int anc_type = ABIL_HEPLIAKLQANA_FIRST_TYPE;
             anc_type <= ABIL_HEPLIAKLQANA_LAST_TYPE;
             ++anc_type)
        {
            abilities.push_back(static_cast<ability_type>(anc_type));
        }
    }

    if (!ignore_silence && silenced(you.pos()))
    {
        if (have_passive(passive_t::wu_jian_wall_jump))
            abilities.push_back(ABIL_WU_JIAN_WALLJUMP);
        return abilities;
    }

    // Remaining abilities are unusable if silenced.
    if (you_worship(GOD_NEMELEX_XOBEH))
    {
        for (int deck = ABIL_NEMELEX_FIRST_DECK;
             deck <= ABIL_NEMELEX_LAST_DECK;
             ++deck)
        {
            abilities.push_back(static_cast<ability_type>(deck));
        }
        if (!you.props[NEMELEX_STACK_KEY].get_vector().empty())
            abilities.push_back(ABIL_NEMELEX_DRAW_STACK);
    }

    for (const auto& power : get_god_powers(you.religion))
    {
        if (god_power_usable(power, ignore_piety, ignore_penance))
        {
            const ability_type abil = fixup_ability(power.abil);
            ASSERT(abil != ABIL_NON_ABILITY);
            abilities.push_back(abil);
        }
    }

    return abilities;
}

void swap_ability_slots(int index1, int index2, bool silent)
{
    // Swap references in the letter table.
    ability_type tmp = you.ability_letter_table[index2];
    you.ability_letter_table[index2] = you.ability_letter_table[index1];
    you.ability_letter_table[index1] = tmp;

    if (!silent)
    {
        mprf_nocap("%c - %s", index_to_letter(index2),
                   ability_name(you.ability_letter_table[index2]));
    }

}

/**
 * What skill affects the success chance/power of a given skill, if any?
 *
 * @param ability       The ability in question.
 * @return              The skill that governs the ability, or SK_NONE.
 */
skill_type abil_skill(ability_type ability)
{
    ASSERT(ability != ABIL_NON_ABILITY);
    return get_ability_def(ability).failure.skill();
}

/**
 * How valuable is it to train the skill that governs this ability? (What
 * 'magnitude' does the ability have?)
 *
 * @param ability       The ability in question.
 * @return              A 'magnitude' for the ability, probably < 10.
 */
int abil_skill_weight(ability_type ability)
{
    ASSERT(ability != ABIL_NON_ABILITY);
    // This is very loosely modelled on a legacy model; fairly arbitrary.
    const int base_fail = get_ability_def(ability).failure.base_chance;
    const int floor = base_fail ? 1 : 0;
    return max(floor, div_rand_round(base_fail, 8) - 3);
}


////////////////////////////////////////////////////////////////////////
// generic_cost

int generic_cost::cost() const
{
    return base + (add > 0 ? random2avg(add, rolls) : 0);
}

int scaling_cost::cost(int max) const
{
    return (value < 0) ? (-value) : ((value * max + 500) / 1000);
}
