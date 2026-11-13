/* ===========================================================================
   DEAD AIR -- the tape content
   '#' inside a transcript line marks a redacted word.
   =========================================================================== */
#include "da.h"
#include "audio_data.h"

const Night NIGHTS[5] = {
    /* ------------------------------------------------------------ TAPE 01 */
    {
        "CONTRACT 96-0114", "TAPE 01", "911 CALL / CASCADE TRAIL 4",
        {
            "ONE CASSETTE, WATER DAMAGED.",
            "RESTORE THE AUDIO. TRANSCRIBE EVERY LINE.",
            "TUNE THE FILTERS UNTIL THE WORDS COME THROUGH.",
            "STAND UP IF THE ROOM GETS LOUD.",
            "PAYMENT ON DELIVERY. DO NOT ARCHIVE A COPY.",
            NULL
        },
        {
            "THE BREATHING DOES NOT STOP WHEN THE TAPE DOES.",
            "IT IS COMING FROM THE CLOSET BESIDE YOU.",
            "YOU DO NOT TURN AROUND. YOU PRESS PLAY.",
            NULL
        },
        CLIP_TAPE1, 5,
        {
            { "NINE ONE ONE. WHAT IS YOUR #EMERGENCY.", { "EMERGENCY" } },
            { "I AM OFF THE TRAIL. I DO NOT KNOW #WHERE.", { "WHERE" } },
            { "THERE IS A SOUND. LIKE THE AIR IS #BREATHING.", { "BREATHING" } },
            { "IT COPIES ME. WHEN I STOP IT KEEPS #GOING.", { "GOING" } },
            { "DO NOT LOOK FOR ME. DO NOT LISTEN TO THIS #TAPE.", { "TAPE" } },
        },
        1, 0.30f, 0.55f, 63.0f, 0.95f, 0.55f, 16.0f, 0x07, 0
    },
    /* ------------------------------------------------------------ TAPE 02 */
    {
        "CONTRACT 96-0203", "TAPE 02", "RITE OF SEPARATION / ST. AGNES",
        {
            "ONE REEL, RECOVERED FROM A BURNT VESTRY.",
            "THE ENTITY ON THIS TAPE IS ACTIVE.",
            "IT SINKS INTO THE ROOM WHEN THE VOICE IS CLEAR.",
            "MATCH ITS FREQUENCY WITH SOMETHING THAT HUMS.",
            "TOO MANY MACHINES WILL BLOW THE MAINS.",
            NULL
        },
        {
            "THE PRIEST STOPS MID-SENTENCE.",
            "SO DOES THE ENTITY. THEN IT CONTINUES,",
            "IN A VOICE THAT IS ONE SEMITONE TOO LOW.",
            "YOU KEEP WORKING.",
            NULL
        },
        CLIP_TAPE2, 6,
        {
            { "THE RECORDER IS #RUNNING, FATHER.", { "RUNNING" } },
            { "COMMAND THE VESSEL TO SPEAK ITS #NAME.", { "NAME" } },
            { "IT GIVES NO NAME. IT GIVES A #NUMBER.", { "NUMBER" } },
            { "SIXTY THREE. IT SINGS IN THE #WALLS.", { "WALLS" } },
            { "EVERY LIGHT. EVERY #FAN. DROWN IT OUT.", { "FAN" } },
            { "IF IT LEARNS THE ROOM, #BURN THE TAPE.", { "BURN" } },
        },
        1, 0.26f, 0.60f, 63.0f, 0.88f, 0.80f, 13.0f, 0x0F, 0
    },
    /* ------------------------------------------------------------ TAPE 03 */
    {
        "CONTRACT 96-0319", "TAPE 03", "USS HALLOWAY / FINAL HOUR",
        {
            "ONE SONAR LOG, SALT CORRODED.",
            "THE ENTITY IS LEARNING YOUR MACHINES.",
            "IT WILL SWITCH THEM OFF TO TRICK YOU.",
            "CHECK THE ROOM. RE-ARM WHAT IT SILENCED.",
            "IGNORE THE DRIPPING. IT IS NOT FROM THE TAPE.",
            NULL
        },
        {
            "THE CEILING ABOVE YOUR DESK IS WET.",
            "THE WATER IS WARM AND IT SMELLS OF DIESEL.",
            "YOU HAVE NEVER BEEN ON A SUBMARINE.",
            NULL
        },
        CLIP_TAPE3, 5,
        {
            { "SONAR REPORTS NO #CONTACT.", { "CONTACT" } },
            { "THEN WHO IS #KNOCKING ON THE HULL.", { "KNOCKING" } },
            { "ONE HUNDRED TWENTY METERS. THE WATER IS #INSIDE US.", { "INSIDE" } },
            { "IT TAPPED BACK. MY BROTHER'S #CODE.", { "CODE" } },
            { "FLOOD THE COMPARTMENT. IT CAN #FOLLOW US UP.", { "FOLLOW" } },
        },
        1, 0.34f, 0.50f, 118.0f, 0.82f, 1.15f, 11.0f, 0x1F, 0
    },
    /* ------------------------------------------------------------ TAPE 04 */
    {
        "CONTRACT 96-0408", "TAPE 04", "INTERVIEW ROOM 3 / SUBJECT UNKNOWN",
        {
            "ONE INTERVIEW CASSETTE, STILL WARM.",
            "THE SUBJECT'S VOICE PRINTS AS YOURS.",
            "THE SESSION IS DATED TOMORROW, 09:40.",
            "WORK FAST. IT HEARS THE KNIVES TURNING.",
            "WHATEVER YOU DO, DO NOT SAY YOUR NAME.",
            NULL
        },
        {
            "THE TIME CODE AT THE FOOT OF THE TAPE",
            "READS 09:40, TOMORROW MORNING.",
            "YOU HAVE NOT COMMITTED THE CRIME YET.",
            "YOU HAVE ONLY AGREED TO IT.",
            NULL
        },
        CLIP_TAPE4, 6,
        {
            { "STATE YOUR #NAME FOR THE RECORD.", { "NAME" } },
            { "YOU ALREADY KNOW IT. YOU PLAYED IT #BACK.", { "BACK" } },
            { "WHERE WERE YOU ON THE NIGHT OF THE #SIXTH.", { "SIXTH" } },
            { "IN MY APARTMENT. A TAPE NOT YET #RECORDED.", { "RECORDED" } },
            { "WHAT DID YOU DO TO THE THING IN THE #WALL.", { "WALL" } },
            { "I TURNED ON EVERYTHING. IT HEARD ME #BREATHING.", { "BREATHING" } },
        },
        1, 0.22f, 0.64f, 47.0f, 0.78f, 1.40f, 9.5f, 0x1F, 0
    },
    /* ------------------------------------------------------------ TAPE 05 */
    {
        "CONTRACT 96-0501", "TAPE 05", "NO LABEL / LIVE FEED",
        {
            "THIS TAPE WAS NOT IN THE COURIER BOX.",
            "IT IS RECORDING YOUR APARTMENT RIGHT NOW,",
            "TEN SECONDS AHEAD OF THE ROOM.",
            "READ THE FEED. ARM THE ROOM IT NAMES.",
            "FINISH THE TRANSCRIPT.",
            NULL
        },
        {
            "THE TAPE RUNS OUT.",
            "THE REELS KEEP TURNING.",
            "THE ROOM IS VERY QUIET, AND THEN,",
            "FROM INSIDE THE MACHINE, YOUR OWN VOICE:",
            "GOOD. NOW LISTEN.",
            NULL
        },
        CLIP_TAPE5, 4,
        {
            { "THIS IS THE ROOM. THIS IS YOU TEN SECONDS #AGO.", { "AGO" } },
            { "DO NOT LOOK BEHIND THE #DESK.", { "DESK" } },
            { "THE MICROWAVE STOPS AT ELEVEN #MINUTES.", { "MINUTES" } },
            { "I AM ALREADY INSIDE THE #MACHINE.", { "MACHINE" } },
        },
        1, 0.28f, 0.58f, 88.0f, 0.74f, 1.70f, 8.0f, 0x1F, 1
    },
};
