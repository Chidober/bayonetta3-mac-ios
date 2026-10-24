// B3 iOS App 入口：设好路径环境变量（缺省都在 App 容器的 Documents 下，devicectl 启动时传的同名变量优先），日志写 Documents/logs/b3_<时间>.log，
// 工作目录切到包里的 boot（模块的 fixups / gates 清单；模块映像本身由 gen_embed.py 编进可执行文件），再调 main_b3.c 的 b3host_main。
// Documents 布局：input/ExeFS、input/RomFS（游戏输入），shader_cache/b-<链>（iOS 版着色器库），shader_map.tsv，save/（存档）。
// B3 iOS App entry : sets the path environment variables (defaults all under the App container's Documents; same-named variables passed by devicectl at launch take precedence), writes the log to Documents/logs/b3_<time>.log,
// changes the working directory to boot in the bundle (the modules' fixups / gates lists; the module images themselves are compiled into the executable by gen_embed.py), then calls b3host_main from main_b3.c.
// Documents layout: input/ExeFS, input/RomFS (game input), shader_cache/b-<chain> (iOS shader library), shader_map.tsv, save/ (save data).
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int b3host_main(int argc, char **argv);

// 程序 ID：照 play_b3.sh 从 ExeFS/main.npdm 取（ACI0 偏移在 0x70，ID 在 ACI0 + 0x10）| program ID: taken from ExeFS/main.npdm as play_b3.sh does (ACI0 offset at 0x70, ID at ACI0 + 0x10)
static void set_program_id(const char *input) {
    char p[2048]; snprintf(p, sizeof p, "%s/ExeFS/main.npdm", input);
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "[b3ios] 缺 %s | missing %s\n", p, p); return; }
    uint32_t o = 0; uint64_t id = 0;
    if (fseek(f, 0x70, SEEK_SET) || fread(&o, 4, 1, f) != 1 || fseek(f, o + 0x10, SEEK_SET) || fread(&id, 8, 1, f) != 1) { fclose(f); return; }
    fclose(f); char v[32]; snprintf(v, sizeof v, "%016llx", (unsigned long long)id); setenv("B3_PROGRAM_ID", v, 0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSString *doc = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
        // 每次启动一个日志 Documents/logs/b3_<时间>.log，只留最近 10 个（之前只留两份，试玩日志会被后来的启动冲掉）| one log per launch in Documents/logs/b3_<time>.log, the latest 10 kept (previously only two were kept and play logs got overwritten by later launches)
        NSString *dir = [doc stringByAppendingPathComponent:@"logs"]; NSFileManager *fm = NSFileManager.defaultManager;
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSArray *old = [[fm contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)];
        for (NSUInteger i = 0; i + 9 < old.count; i++) [fm removeItemAtPath:[dir stringByAppendingPathComponent:old[i]] error:nil];
        NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"yyyyMMdd_HHmmss";
        NSString *log = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"b3_%@.log", [df stringFromDate:NSDate.date]]];
        freopen(log.fileSystemRepresentation, "w", stderr); setvbuf(stderr, NULL, _IOLBF, 0); dup2(fileno(stderr), 1);
        #define DOC(name, sub) setenv(name, [doc stringByAppendingPathComponent:sub].fileSystemRepresentation, 0)
        DOC("B3_INPUT", @"input"); DOC("B3_SAVE_DIR", @"save"); DOC("B3_SHADER_LIB", @"shader_cache"); DOC("B3_SHADER_MAP", @"shader_map.tsv");
        setenv("DM_B3_LENIENT", "1", 0); setenv("B3_WINDOW", "1", 0);
        DOC("DM_PSO_USER", @"pso_user.tsv");   // 管线预热清单：玩过的场景下次启动在后台先建好 | pipeline warm-up list: scenes played before are prebuilt in the background on the next launch
        set_program_id(getenv("B3_INPUT"));
        // B3_BENCH=1（./b3 bench --ios 启动时传）：用 Documents/bench/save0 的新副本当存档，回放 Documents/bench/input.tsv，放完自动退出 | B3_BENCH=1 (passed by ./b3 bench --ios): use a fresh copy of Documents/bench/save0 as the save, replay Documents/bench/input.tsv, exit when done
        if (getenv("B3_BENCH") && !strcmp(getenv("B3_BENCH"), "1")) {
            NSString *b = [doc stringByAppendingPathComponent:@"bench"], *run = [b stringByAppendingPathComponent:@"save_run"];
            [NSFileManager.defaultManager removeItemAtPath:run error:nil];
            [NSFileManager.defaultManager copyItemAtPath:[b stringByAppendingPathComponent:@"save0"] toPath:run error:nil];
            setenv("B3_SAVE_DIR", run.fileSystemRepresentation, 1); setenv("B3_INPUT_PLAY", [b stringByAppendingPathComponent:@"input.tsv"].fileSystemRepresentation, 1);
            setenv("B3_REPLAY_EXIT", "1", 1); setenv("B3_MUTE", "1", 0);
        }
        [NSFileManager.defaultManager createDirectoryAtPath:@(getenv("B3_SAVE_DIR")) withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *boot = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"boot"];
        if (chdir(boot.fileSystemRepresentation)) { fprintf(stderr, "[b3ios] chdir %s 失败 | failed\n", boot.fileSystemRepresentation); return 1; }
        fprintf(stderr, "[b3ios] Documents %s，程序 ID %s | program ID\n", doc.fileSystemRepresentation, getenv("B3_PROGRAM_ID") ?: "?");
    }
    char *av[] = {"b3host", ".", "main", "subsdk0", "sdk", NULL};
    return b3host_main(5, av);
}
