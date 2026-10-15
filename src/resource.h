#pragma once

// Icon. Id 1 so Explorer picks it as the application icon.
#define IDI_APPMAIN                1

// Dialogs
#define IDD_MAIN                 100
#define IDD_SESSION              101
#define IDD_NAME                 102
#define IDD_BLOCK_APP            103
#define IDD_BLOCK_DEVICE         104
#define IDD_BLOCK_GAIN           105
#define IDD_ABOUT                106
#define IDD_BLOCK_DENOISE        107
#define IDD_BLOCK_RECORDER       108
#define IDD_BLOCK_TONE           109
#define IDD_BLOCK_EQ10           110
#define IDD_BLOCK_CHANNELS       111
#define IDD_ERRORLOG             112
#define IDD_BLOCK_SYSTEM         113
#define IDD_SETTINGS             114
#define IDD_HOTKEY               115
#define IDD_BLOCK_STREAM         116
#define IDD_SCHEDULE             117
#define IDD_BLOCK_COMP           118
#define IDD_BLOCK_LIMIT          119
#define IDD_BLOCK_PLUGIN         120
#define IDD_BLOCK_GATE           121
#define IDD_BLOCK_PARA           122
#define IDD_BLOCK_NORM           123
#define IDD_BLOCK_CLIPMON        124
#define IDD_BLOCK_BUFFER         125
#define IDD_BLOCK_ACTIONS        126
#define IDD_BLOCK_PITCH          127
#define IDD_BLOCK_NETRECV        128
#define IDD_BLOCK_NETSEND        129
#define IDD_BLOCK_ASIO           131
#define IDD_NET_OPUS             130

// Menus
#define IDR_MAIN_MENU            200
#define IDR_SESSION_MENU         201
#define IDR_MAIN_CTX             202
#define IDR_BLOCK_CTX            203
#define IDR_LIB_CTX              204
#define IDR_TRAY_CTX             205
#define IDR_SCHED_CTX            206

// Main window controls
#define IDC_SESSION_LIST        1000
#define IDC_MAIN_TABS           1001
#define IDC_SESSIONS_LABEL      1002
#define IDC_SCHED_LIST          1003
#define IDC_SCHED_LABEL         1004
// A read only edit rather than a static: a screen reader can walk an edit
// field a line at a time, which a label does not allow.
#define IDC_MAIN_RUNNING        1005
#define IDC_MAIN_RUNNING_LABEL  1006

// Session window controls
#define IDC_LIB_TREE            1010
#define IDC_BLOCK_LIST          1011
#define IDC_SESSION_STATE       1020
#define IDC_SESSION_TIMES       1021
#define IDC_SESSION_TIMES_LABEL 1022

// Property dialog controls
#define IDC_NAME_EDIT           1030
#define IDC_APP_TREE            1032
#define IDC_DEV_COMBO           1033
#define IDC_VOL_SLIDER          1034
#define IDC_VOL_TEXT            1035
#define IDC_BTN_REFRESH         1036
#define IDC_DEV_LABEL           1037
#define IDC_NAME_LABEL          1038
#define IDC_ABOUT_TEXT          1039
#define IDC_BLOCK_ENABLED       1040
#define IDC_MIX_SLIDER          1042
#define IDC_MIX_TEXT            1043
#define IDC_SUP_SLIDER          1044
#define IDC_SUP_TEXT            1045
#define IDC_SUP_LABEL           1046
#define IDC_REC_FOLDER          1047
#define IDC_REC_BROWSE          1048
#define IDC_REC_TEMPLATE        1049
#define IDC_REC_FORMAT          1050
#define IDC_REC_KBPS            1051
#define IDC_REC_KBPS_LABEL      1052
#define IDC_REC_TOKENS          1053
#define IDC_BASS_SLIDER         1054
#define IDC_BASS_TEXT           1055
#define IDC_TREB_SLIDER         1056
#define IDC_TREB_TEXT           1057
#define IDC_EQ_RESET            1058
#define IDC_WIDTH_SLIDER        1059
#define IDC_WIDTH_TEXT          1060
#define IDC_CHAN_SWAP           1061
#define IDC_CHAN_CENTRE         1062
#define IDC_CHAN_NOTE           1063
#define IDC_LOG_TEXT            1064
#define IDC_LOG_COPY            1065
#define IDC_LOG_CLEAR           1066
#define IDC_REC_GATE            1067
#define IDC_REC_GATE_SLIDER     1068
#define IDC_REC_GATE_TEXT       1069
#define IDC_REC_GATE_LABEL      1070
#define IDC_REC_HOLD_SLIDER     1071
#define IDC_REC_HOLD_TEXT       1072
#define IDC_REC_HOLD_LABEL      1073
#define IDC_REC_SPLIT           1074
#define IDC_REC_SPLIT_VALUE     1075
#define IDC_REC_SPLIT_UNITS     1076
#define IDC_REC_SPLIT_LABEL     1077
#define IDC_SYS_EXCLUDE         1078
#define IDC_SYS_ADD             1079
#define IDC_SYS_NOTE            1080
#define IDC_HK_ENABLED          1081
#define IDC_HK_CTRL             1082
#define IDC_HK_ALT              1083
#define IDC_HK_SHIFT            1084
#define IDC_HK_WIN              1085
#define IDC_HK_KEY              1086
#define IDC_HK_NOTE             1087
#define IDC_HK_WHAT             1088
#define IDC_STR_URL             1089
#define IDC_STR_BUFFER          1090
#define IDC_STR_BUFFER_TEXT     1091
#define IDC_STR_RECONNECT       1092
#define IDC_STR_RETRY           1093
#define IDC_STR_RETRY_LABEL     1094
#define IDC_STR_NOTE            1095
#define IDC_SCH_SESSION         1096
#define IDC_SCH_MODE_TIME       1097
#define IDC_SCH_MODE_IN         1098
#define IDC_SCH_HOUR            1099
#define IDC_SCH_MIN             1130
#define IDC_SCH_INMIN           1131
#define IDC_SCH_REPEAT          1132
#define IDC_SCH_DOW             1133
#define IDC_SCH_DOW_LAST        1139
#define IDC_SCH_STOP            1140
#define IDC_SCH_STOPHOUR        1141
#define IDC_SCH_STOPMIN         1142
#define IDC_SCH_AFTER           1143
#define IDC_SCH_SINGLE          1144
#define IDC_SCH_ENABLED         1145
#define IDC_SCH_TIMELABEL       1146
#define IDC_SCH_INLABEL         1147
#define IDC_SCH_STOPLABEL       1148
#define IDC_SCH_AFTERLABEL      1149
#define IDC_SCH_DOWLABEL        1150

// Compressor
#define IDC_COMP_THRESH         1151
#define IDC_COMP_THRESH_TEXT    1152
#define IDC_COMP_RATIO          1153
#define IDC_COMP_RATIO_TEXT     1154
#define IDC_COMP_ATTACK         1155
#define IDC_COMP_ATTACK_TEXT    1156
#define IDC_COMP_RELEASE        1157
#define IDC_COMP_RELEASE_TEXT   1158
#define IDC_COMP_AUTO           1159
#define IDC_COMP_MAKEUP         1160
#define IDC_COMP_MAKEUP_TEXT    1161
#define IDC_COMP_MAKEUP_LABEL   1162

// Limiter
#define IDC_LIM_CEIL            1163
#define IDC_LIM_CEIL_TEXT       1164
#define IDC_LIM_RELEASE         1165
#define IDC_LIM_RELEASE_TEXT    1166
#define IDC_LIM_LOOK            1167
#define IDC_LIM_LOOK_TEXT       1168
#define IDC_LIM_NOTE            1169

// Application input block: where it gets its target from
#define IDC_APP_MODE_NAMED      1170
#define IDC_APP_MODE_FRONT      1171
#define IDC_APP_MODE_FOLLOW     1172
#define IDC_APP_LABEL           1173
#define IDC_APP_NOTE            1174

// Settings: plugin folders
#define IDC_PLUG_LABEL          1175
#define IDC_PLUG_LIST           1176
#define IDC_PLUG_ADD            1177
#define IDC_PLUG_REMOVE         1178
#define IDC_PLUG_SCAN           1179
#define IDC_PLUG_FOUND          1180

// One plugin block's parameters
#define IDC_PP_NAME             1181
#define IDC_PP_LIST             1182
#define IDC_PP_VALUE_LABEL      1183
#define IDC_PP_SLIDER           1184
#define IDC_PP_CHOICE           1185
#define IDC_PP_TEXT             1186
#define IDC_PP_EDITOR           1187
#define IDC_PP_RESET            1188

// Noise gate
#define IDC_GATE_THRESH         1189
#define IDC_GATE_THRESH_TEXT    1190
#define IDC_GATE_RANGE          1191
#define IDC_GATE_RANGE_TEXT     1192
#define IDC_GATE_ATTACK         1193
#define IDC_GATE_ATTACK_TEXT    1194
#define IDC_GATE_HOLD           1195
#define IDC_GATE_HOLD_TEXT      1196
#define IDC_GATE_RELEASE        1197
#define IDC_GATE_RELEASE_TEXT   1198
#define IDC_GATE_NOTE           1199

// Four band parametric EQ
#define IDC_PEQ_LIST            1200
#define IDC_PEQ_TYPE            1201
#define IDC_PEQ_TYPE_LABEL      1202
#define IDC_PEQ_FREQ            1203
#define IDC_PEQ_FREQ_TEXT       1204
#define IDC_PEQ_FREQ_LABEL      1205
#define IDC_PEQ_GAIN            1206
#define IDC_PEQ_GAIN_TEXT       1207
#define IDC_PEQ_GAIN_LABEL      1208
#define IDC_PEQ_Q               1209
#define IDC_PEQ_Q_TEXT          1210
#define IDC_PEQ_Q_LABEL         1211
#define IDC_PEQ_RESET           1212

// Default recording folder, in Settings
#define IDC_SET_REC_LABEL       1213
#define IDC_SET_REC_FOLDER      1214
#define IDC_SET_REC_BROWSE      1215
#define IDC_SET_UPDATE_LABEL    1320
#define IDC_SET_UPDATE          1321
#define IDC_SET_UPDATE_SILENT   1322

// Where a session or block hotkey works: everywhere, or only in FastRoute
#define IDC_HK_SCOPE_GLOBAL     1216
#define IDC_HK_SCOPE_WINDOW     1217

// Normaliser
#define IDC_NORM_TARGET         1218
#define IDC_NORM_TARGET_TEXT    1219
#define IDC_NORM_LOOK           1220
#define IDC_NORM_LOOK_TEXT      1221
#define IDC_NORM_MAXGAIN        1222
#define IDC_NORM_MAXGAIN_TEXT   1223
#define IDC_NORM_RELEASE        1224
#define IDC_NORM_RELEASE_TEXT   1225
#define IDC_NORM_NOTE           1226

// Clipping monitor
#define IDC_CLIP_THRESH         1227
#define IDC_CLIP_THRESH_TEXT    1228
#define IDC_CLIP_INTERVAL       1229
#define IDC_CLIP_INTERVAL_TEXT  1230
#define IDC_CLIP_NOTE           1231

// Buffer
#define IDC_BUF_SECONDS         1232
#define IDC_BUF_SECONDS_TEXT    1233
#define IDC_BUF_STEP            1234
#define IDC_BUF_STEP_TEXT       1235
#define IDC_BUF_ACTIONS         1236
#define IDC_BUF_NOTE            1237

// Action hotkeys, for the blocks that have actions
#define IDC_ACT_LIST            1238
#define IDC_ACT_SET             1239
#define IDC_ACT_CLEAR           1240
#define IDC_ACT_NOTE            1241

// Channel mixer, per channel volume and pan
#define IDC_CHAN_LVOL           1242
#define IDC_CHAN_LVOL_TEXT      1243
#define IDC_CHAN_LPAN           1244
#define IDC_CHAN_LPAN_TEXT      1245
#define IDC_CHAN_RVOL           1246
#define IDC_CHAN_RVOL_TEXT      1247
#define IDC_CHAN_RPAN           1248
#define IDC_CHAN_RPAN_TEXT      1249
#define IDC_CHAN_RESET          1250

// Apply, on every block parameter dialog. One id, because they all mean
// the same thing by it.
#define IDC_APPLY               1251

#define IDC_GATE_LOOK           1252
#define IDC_GATE_LOOK_TEXT      1253

#define IDC_PITCH_SEMIS         1254
#define IDC_PITCH_SEMIS_TEXT    1255
#define IDC_PITCH_CENTS         1256
#define IDC_PITCH_CENTS_TEXT    1257
#define IDC_PITCH_QUALITY       1258
#define IDC_PITCH_SUMMARY       1259
#define IDC_PITCH_NOTE          1260

// Net Receive
#define IDC_NR_PROTOCOL         1261
#define IDC_NR_FOUND            1262
#define IDC_NR_REFRESH          1263
#define IDC_NR_ADDRESS          1264
#define IDC_NR_PORT             1265
#define IDC_NR_NAME             1266
#define IDC_NR_BUFFER           1267
#define IDC_NR_BUFFER_TEXT      1268
#define IDC_NR_NOTE             1269

// Net Send
#define IDC_NS_PROTOCOL         1270
#define IDC_NS_MODE             1271
#define IDC_NS_ADDRESS          1272
#define IDC_NS_ADDRESS_LABEL    1273
#define IDC_NS_PORT             1274
#define IDC_NS_NAME             1275
#define IDC_NS_CODEC            1276
#define IDC_NS_CODEC_LABEL      1277
#define IDC_NS_PACKET           1278
#define IDC_NS_PACKET_TEXT      1279
#define IDC_NS_KBPS             1280
#define IDC_NS_KBPS_LABEL       1281
#define IDC_NS_KBPS_TEXT        1282
#define IDC_NS_TTL              1283
#define IDC_NS_TTL_TEXT         1284
#define IDC_NS_NOTE             1285
#define IDC_NS_TTL_LABEL        1286
#define IDC_NS_OPUSMORE         1287

// The rest of what Opus can be told, on a dialog of its own
#define IDC_NO_LOSS             1288
#define IDC_NO_LOSS_TEXT        1289
#define IDC_NO_CPLX             1290
#define IDC_NO_CPLX_TEXT        1291
#define IDC_NO_SIGNAL           1292
#define IDC_NO_RATEMODE         1293
#define IDC_NO_DTX              1294
#define IDC_NO_NOTE             1295
#define IDC_AS_DRIVER           1300
#define IDC_AS_REFRESH          1301
#define IDC_AS_LEFT             1302
#define IDC_AS_RIGHT            1303
#define IDC_AS_LEFT_LABEL       1304
#define IDC_AS_RIGHT_LABEL      1305
#define IDC_AS_PANEL            1306
#define IDC_AS_NOTE             1307
#define IDC_APP_LIST            1330
#define IDC_APP_ADD             1331
#define IDC_APP_ALL             1332
#define IDC_APP_WAIT            1333
#define IDC_REC_MODE            1334
#define IDC_REC_MODE_NOTE       1335
#define IDC_REC_RATE            1336
#define IDC_REC_RATE_LABEL      1337
#define IDC_REC_BITS            1338
#define IDC_REC_BITS_LABEL      1339

// Ten band EQ: one slider and one readout per band, contiguous so the band
// index is just the control id minus the base.
#define IDC_EQ_SLIDER_BASE      1100
#define IDC_EQ_TEXT_BASE        1120

// Main window commands
#define IDM_SESSION_NEW         40001
#define IDM_SESSION_OPEN        40002
#define IDM_SESSION_RENAME      40003
#define IDM_SESSION_DELETE      40004
#define IDM_SESSION_RUN         40005
#define IDM_SESSION_STOP        40006
#define IDM_APP_EXIT            40007
#define IDM_HELP_ABOUT          40008
#define IDM_HELP_UPDATE         40036
#define IDM_SESSION_TOGGLE      40009
#define IDM_SESSION_AUTORUN     40010
#define IDM_HELP_ERRORS         40011
#define IDM_APP_HIDE            40012
#define IDM_APP_SETTINGS        40013
#define IDM_TRAY_SHOW           40014
#define IDM_SESSION_HOTKEY      40015
#define IDM_SCHED_NEW           40016
#define IDM_SCHED_EDIT          40017
#define IDM_SCHED_DELETE        40018
#define IDM_SCHED_TOGGLE        40019

// Session window commands
#define IDM_BLOCK_ADD           40020
#define IDM_BLOCK_EDIT          40021
#define IDM_BLOCK_REMOVE        40022
#define IDM_BLOCK_UP            40023
#define IDM_BLOCK_DOWN          40024
#define IDM_SESS_RUN            40025
#define IDM_SESS_STOP           40026
#define IDM_SESS_CLOSE          40027
#define IDM_SESS_TOGGLE         40028
#define IDM_BLOCK_TOGGLE        40029
#define IDM_BLOCK_VOL_UP        40030
#define IDM_BLOCK_VOL_DOWN      40031
#define IDM_BLOCK_HOTKEY        40032
#define IDM_BLOCK_RENAME        40033
#define IDM_SESSION_OPENDIR     40034
#define IDM_BLOCK_ACTIONS       40035
#define IDM_BLOCK_CUT           40036
#define IDM_BLOCK_COPY          40037
#define IDM_BLOCK_PASTE         40038
#define IDM_BLOCK_PASTE_ABOVE   40039
