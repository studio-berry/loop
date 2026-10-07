Category: added
Audience: operators
Breaking-Change: no
Summary: A completed page-box drag now runs on a default install. Loop ships a page-box move recipe embedded in the application, so the Fix workspace plans and executes the move without importing a recipe. An operator recipe that offers translate-page-box takes precedence, and an invalid operator recipe stops the move rather than falling back to the shipped one.
