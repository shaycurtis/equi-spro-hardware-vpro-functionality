# Cloud de-duplication, parked

PR: https://github.com/shaycurtis/equi-spro-hardware-vpro-functionality/pull/2

The code is open and not deployed. The field Lambda is unchanged.

## Blocker

`AWSReservedSSO_AdministratorAccess` for shay@equimetrics.ie cannot create `vpro-dev-ble-dedup` in eu-west-1. The error is that no service control policy allows `dynamodb:CreateTable`. An organization management account has to allow that action for this account. The table has to be in the same account as `postBleData`.

## After the policy allows it

1. Create DynamoDB table `vpro-dev-ble-dedup`. Partition key `device` (string), sort key `sk` (string), on-demand, TTL attribute `ttl`.
2. Allow the postBleData role `dynamodb:GetItem`, `PutItem`, `UpdateItem`, `DeleteItem`, and `Query` on that table.
3. Build with `python cloud/post_ble_data/build.py --python <the postLoraData runtime> --no-layer` and upload `cloud/post_ble_data/dist/post_ble_data.zip`. Handler stays `post_ble_data.lambda_handler`.
4. Regenerate the sample with `python cloud/post_ble_data/make_sample_event.py` on the PC that has `private/fleet_key.bin`. First invoke should show `horseTemperature` 37.50. Second invoke should show `duplicates` 1 and `posted` 0.

The commercial gateway payload adapter is still later.
