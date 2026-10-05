Category: added
Audience: operators, integrators
Breaking-Change: no
Summary: A completed drag of a page box is now committed through the command catalog instead of being discarded. DragSession carries the revision fence it completed against; EditorHost discards a drag whose fence went stale and otherwise invokes the new actionMoveSelection command (108 shell actions), which proposes a bound translate-page-box correction in the Fix workspace for plan, approval and execute. translate-page-box is a new Core repair operation that moves one page box on one page, saves as a new artifact, and refuses a move that leaves the box nesting invalid. A recipe offering translate-page-box must be imported to execute a move; none ships built in.
