HOW TO BUILD
----------------------------------------------
Install Rexglue-SDk following the <a href="https://github.com/rexglue/rexglue-sdk/wiki">wik</a>
<br>
install Visual Studio Community edition and ensure you install the desktop development with C++ and make sure you check the box that says C++ clang compiler for windows (note: if you are using Mac or linux you can skip this for you will have to follow the wiki linked in step 1 to build the game)
<br>
clone/download the repository
<br>
dump your copy of Lollipop: Chainsaw (Xbox360) and use a tool like ISO-Extract to dump the contents of the iso (INCLUDING THE DEFAULT.XEX FILE)
<br>
place the contents of the iso in the assets folder(INCLUDING THE DEFAULT.XEX FILE)
<br>
open the folder in visual studio, go into cmake targets view
<br>
change the configuration to win-amd64-relwithdebinfo
<br>
put rexglue.exe in your path environment variable and do rexglue codegen redahm_config.toml in a terminal (visual studios works, or you can use windows default terminal/cmd/powershell)
<br>
right click re_cherry project and select build all
<br>
copy the assets folder with the dumped contents of the iso in out/build/win-amd64-relwithdebinfo

HOW TO USE
----------------------------------
for the time being until a launcher is completed all you must do is download the repo and go into out/build/win-amd64-relwithdebinfo and either put the assets folder with the dumped assets, redahm.toml, and the default.xex in it or make a new folder somewhere and place the assets with the default.xex in there the only folder/files you should have are the kronosgame folder with its files and the default.xex

CURRENT ISSUES WITH THE GAME
Screen-Tearing 
IF YOU FIND ANY CRASHES PLEASE MAKE AN ISSUE EXPLAINING WHERE IT WAS AND WHAT YOU WERE DOING (i.e. went to high-school level, game crashed during loading or nick-toss on enemy causing the game to crash)(no these dont happen anymore they're just examples)

CREDITS
MadLadMikael - for helping setup and teaching howto use REXGLUESDK and GITHUB

