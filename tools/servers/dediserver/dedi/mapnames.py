"""Driver 2 multiplayer map names, shared by vote and rotation announcements.

Arenas 0 and 1 are the stock per-city frontend list (FEscreens.inc screens
35-38). Vegas 2 and 3 are the cut Ghost Town / North Vegas regions
(EN_MISSION.LTXT, FEmain.c gameAreas for 2-player Checkpoint).
A city/arena with no name falls back to "mp N".
"""

CITIES = ("chicago", "havana", "vegas", "rio")

# (city, arena) -> name
TITLES = {
    (0, 0): "River North",
    (0, 1): "Cabrini Green",
    (1, 0): "Plaza de Revolucion",
    (1, 1): "Old Havana",
    (2, 0): "Downtown",
    (2, 1): "The Lakes",
    (2, 2): "Ghost Town",
    (2, 3): "North Vegas",
    (3, 0): "Centro",
    (3, 1): "Flamengo",
}


def arena_title(city, arena):
    return TITLES.get((city, arena))


def where(city, mode, arena):
    """Player-facing 'CHICAGO River North' / 'VEGAS city' / 'VEGAS mp 4'."""
    city_name = CITIES[city].upper() if 0 <= city < len(CITIES) else "?"
    if mode == "city":
        return "%s city" % city_name
    title = arena_title(city, arena)
    if title:
        return "%s %s" % (city_name, title)
    return "%s mp %d" % (city_name, arena)
