# VolunTree

VolunTree connects volunteers and NGOs through user profiles, posts, and friend
lists.

## Requirements

- Node.js 18 or newer
- MongoDB (local or hosted)

## Setup

1. Copy `server/.env.example` to `server/.env` and set `MONGO_URI` and a
   private `JWT_SECRET`. Set `GMAIL_USER` and `GMAIL_PASS` to enable password
   reset emails.
   The client defaults to the local API address; copy
   `client/.env.example` to `client/.env` only if you need to change it.
2. Install and start the API:

   ```sh
   cd server
   npm install
   npm run dev
   ```

3. In a separate terminal, install and start the client:

   ```sh
   cd client
   npm install
   npm start
   ```

The API listens on port 5000 by default and the client on port 3000. To use a
different API address, set `REACT_APP_API_URL` in the client environment (for
example, `http://localhost:5000`).
