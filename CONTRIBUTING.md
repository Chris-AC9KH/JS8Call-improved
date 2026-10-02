#  JS8Call Developer and Contributor Guidelines
-   **Basic Overview of Project Structure**
    The JS8Call project's main branch (sometimes called "master") is the branch to which PR's for all new features
    and enhancements should be submitted. It should be considered an unstable development branch, not suitable to
    build and run except for development purposes. It will likely be in a constant state of flux as new PR's are
    merged and tested.

    Stable release branches are prefixed with `release/` followed by the release version number, and these may also
    include release candidate versions of a pending stable production release

    The project maintainer core team is responsible for reviewing and merging PR's into the project. There is no set
    schedule for major or minor point releases. It is up to the project maintainer(s) to decide when and if main is
    worthy of a new release. However, the lifespan of a -rc (release candidate) should be generally considered to be
    no more than 30 days from the release of -rc1 for final debugging and testing before it goes to a stable production
    release.

#  How to submit a PR
-   Before starting work on the code make sure you checkout main on your local repo and git pull any changes that may
    have been merged since the last time you updated it. It is recommended to create a development branch from main
    with git checkout -b 'new_branch'. You can call it anything you wish, but the idea is always to keep your local
    main branch in sync with upstream and only make changes to your development branches. There may be other developers
    making changes to the code in the same files you are working on. Keeping your local main branch up-to-date with
    upstream will make it much easier to rebase your development branch on main before submitting a PR.

    Remember - it is the developer's responsibility to make sure your changes to the code build and run, and that all
    conflicts are resolved before submitting a PR.

    If your PR is a work-in-process where you may want peer code review or submissions from other contributors, label
    the PR with `WIP - 'TITLE OF PR'` so project maintainers can skip over it when merging other PR's without wasting
    time to determine if it is suitable for merge into the project. In addition it is recommended to label your PR
    with `Feature` if you are adding a new feature. `Bug Fix` if your PR is related to such. `Doc` if you are submitting
    documentation. `Enhancement` if you are submitting an improvement that is none of the above. If applicable, if there
    is an issue in the issue list related to your PR you should reference the issue number.

#  Creating a clean commit history
-   If your PR contains a number of commits it is recommended to use interactive rebasing on your local branch prior
    to submitting a PR and pushing it to your github repo from which the PR will be created. For instance you may have
    a half dozen or more commits all related to the same changes to the code. Those commits should be squashed into one.
    Say there is six commits related to a bug fix or feature, you can combine them into one commit with:
    `git rebase -i HEAD~6` and change the pick column accordingly to squash them and combine the commit comments.

    The final step before creating a PR is to rebase your development branch on master, resolve any conflicts, build it
    and make sure it runs.

    If you are not familiar with rebasing, refer to the git documentation at
    https://git-scm.com/docs/git-rebase

    Remember - it is not the project maintainer(s) responsibility to build and test your PR. They expect you to submit
    good code that builds and runs. There may be bugs found in it later that need to be patched but the maintainer(s)
    do not have time to track down bad code that breaks main, or won't build and run.

#  Bug fixes
-   The whole purpose of a release candidate is final debugging by both end users and developers before it goes to a stable
    production release. If you submit a bug fix PR to a release candidate branch it is likely the same bug exists in main
    and a PR should be submitted to main as well. The same applies the other way around - if you are working on main and
    squash a bug, and the same code exists in a release candidate branch, a PR for the bug fix should be submitted to both
    branches.

    Under no circumstances submit PR's to a release candidate branch to get your latest and greatest feature into it, change
    libraries, etc.. New features go to main, only bug fixes are submitted to a release candidate.

#  Where code belongs
-   **Read the code documentation first.**
    The generated documentation at https://js8call.com/JS8Call-improved/ describes what each class does and which of its
    member functions own which job. Check it before adding code. To read it offline, generate it from your clone with
    `.github/workflows/misc/Doxyfile`; `docs/DOXYGEN.md` shows how.

-   **Extend before you add.**
    Before writing a new function, check whether an existing one already owns the concern and extend that instead; do not
    create a second path for the same job. A new function needs a reason, such as more than one caller or moving code out
    of a large file (see below). New API code, commands and outbound events alike, belongs in
    `UI_Constructor::networkMessage()` (`JS8_Mainwindow/networkMessage.cpp`).

-   **The largest hand-written files are closed to substantial new code.**
    `JS8_UI/mainwindow.cpp`, `JS8_UI/Configuration.cpp` and `JS8_Mode/JS8.cpp` have grown to the point where adding to them
    makes them harder to maintain and harder to review. If a change wants more than a few dozen lines in any of these,
    extract the new code before you write it, not after review. Editing logic that is already there is fine; adding a new
    body of it is not.

    There are two established places for it, both already visible in the directory layout.

-   **Window logic that needs the window's own state goes in `JS8_Mainwindow/`.**
    One file per member function, named after the function - `processRxActivity.cpp`, `displayCallActivity.cpp`,
    `pushNotificationHandler.cpp` and fourteen more. Declare the method in `JS8_UI/mainwindow.h`, define it in its own file
    under `JS8_Mainwindow/`, and add the file to `CMakeLists.txt`.

-   **Code that owns state of its own goes in a class under `JS8_Main/`.**
    Add `JS8_Main/<ClassName>.{h,cpp}`, hold it as an owning member of whatever uses it, delegate to it in a single line at
    the call site, and add the `.cpp` to `CMakeLists.txt`. `Inbox`, `APRSISClient` and `Bands` are existing examples. Prefer
    this whenever the new code has data of its own to keep - reserve a `JS8_Mainwindow/` member for logic that genuinely
    needs the window's guts.

#  Documentation
-   **Document what you add, and keep functions small enough to review.**
    Every new or changed function included in the generated documentation gets a Doxygen `/** */` block. An undocumented
    override may inherit its base declaration's block, and excluded implementation details need a block only where it
    helps a maintainer. A large function is split into helpers rather than submitted as one long body for a reviewer to
    work through. `docs/DOXYGEN.md` covers what this project's Doxyfile expects of a block, and how to check yours locally
    before submitting it.
