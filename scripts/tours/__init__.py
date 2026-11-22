"""The scripted tours through the game, driven through the control bridge (scripts/longshot_bridge.py). The entry points
are scripts/pipeline_tour.py and scripts/memtour.py; this package holds their parts:

    route      the pipeline tour's route: the Debug tab's actions read from the source, the stops, the presses
    drive      a game driven along the route: Game (one bridge), teleports and the age switch, a new game, play
    memory     memory from /proc: the game's and the machine's, and the pipeline tour's memory guard
    lists      the pipeline state lists: merge, compare, and what each step of a run is worth (value)
    vkprofile  the renderer's play profile recorded on the route (--vk-profile) and merged (merge-profile)
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # (the checkout)


