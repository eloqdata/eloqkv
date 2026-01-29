# USAGE:
# python3 log_replay_test.py --load
# python3 log_replay_test.py --verify
# diff database_snapshot_before_replay.json database_snapshot_after_replay.json

import argparse
import redis
import random
import json
import threading
import logging
from concurrent.futures import ThreadPoolExecutor

# DEBUG/INFO/WARNING/ERROR
logging.basicConfig(level=logging.WARNING)  # Set level to DEBUG for debugging


string_modifiable_commands = [
    "SET",
    "SETNX",
    "GETSET",
    "PSETEX",
    "SETEX",
    "INCRBYFLOAT",
    "SETBIT",  # temporirily disabled due to printing crash
    "SETRANGE",
    "APPEND",
    "INCR",
    "INCRBY",
    "DECR",
    "DECRBY",
]

string_modifiable_commands_multi_object = [
    "MSET",
    "MSETNX",
]

string_readonly_commands = [
    "GET",
    "MGET",
    "STRLEN",
    "GETBIT",
    "GETRANGE",
    "SUBSTR",
]

list_modifiable_commands = [
    "LINSERT",  # Insert an element before or after a pivot
    "LPOP",  # Remove and return the first element from a list
    "LPUSH",  # Prepend an element to the beginning of a list
    "LPUSHX",  # Prepend an element to a list, only if it exists
    "LREM",  # Remove elements matching a value
    "RPUSH",  # Append an element to a list
    "RPUSHX",  # Append an element to a list, only if it exists
    "LSET",  # Set a specific element's value
    "RPOP",  # Remove and return the last element from a list
    "LTRIM",  # Trim a list to a specified range
]

list_modifiable_commands_multi_object = [
    "LMOVE",
    # "RPOPLPUSH",
    # "BRPOPLPUSH",  # Blocking pop from one list and push to another
    # "LMPOP",
]

list_readonly_commands = ["LINDEX", "LPOS", "LLEN", "LRANGE"]

hash_modifiable_commands = [
    "HSET",  # Set the string value of a hash field
    "HINCRBY",  # Increment the integer value of a hash field by a given amount
    "HINCRBYFLOAT",  # Increment the float value of a hash field by a given amount
    "HDEL",  # Delete one or more hash fields
    "HMSET",  # Set multiple hash fields to multiple values
    # "HSETNX",
]

hash_readonly_commands = [
    "HGET",
    "HLEN",
    "HSTRLEN",
    "HMGET",
    "HKEYS",
    "HVALS",
    "HGETALL",
    "HEXISTS",
    "HRANDFIELD",
    "HSCAN",
]

zset_modifiable_commands = [
    "ZADD",  # Add one or more members to a sorted set, or update their scores
    "ZREM",  # Remove one or more members from a sorted set
    "ZREMRANGE",  # Remove all members in a sorted set within the given indexes
    "ZPOPMIN",  # Remove and return members with the lowest scores from one or more sorted sets
    "ZPOPMAX",  # Remove and return members with the highest scores from one or more sorted sets
    # "ZINCRBY",
]

zset_modifiable_commands_multi_object = [
    "ZUNIONSTORE",
    "ZINTERSTORE",
    "ZDIFFSTORE",
    # "ZMPOP",
]

zset_readonly_commands = [
    "ZCOUNT",
    "ZCARD",
    "ZRANGE",
    "ZSCORE",
    "ZRANGEBYSCORE",
    "ZRANGEBYLEX",
    "ZRANGEBYRANK",
    "ZLEXCOUNT",
    "ZSCAN",
    "ZUNION",
    "ZRANDMEMBER",
    "ZRANK",
    "ZREVRANK",
    "ZREVRANGE",
    "ZREVRANGEBYSCORE",
    "ZREVRANGEBYLEX",
    "ZMSCORE",
    "ZDIFF",
]

set_modifiable_commands = [
    "SADD",  # Add one or more members to a Set
    "SPOP",  # Remove and return a random member from a Set
    "SREM",  # Remove one or more members from a Set
]

set_modifiable_commands_multi_object = [
    "SDIFFSTORE",  # Store the difference between Sets
    "SINTERSTORE",  # Store the intersection of Sets
    "SUNIONSTORE",  # Store the union of Sets
    "SMOVE",
]

set_readonly_commands = [
    "SMEMBERS",
    "SCARD",
    "SDIFF",
    "SINTER",
    "SINTERCARD",
    "SISMEMBER",
    "SMISMEMBER",
    "SRANDMEMBER",
    "SUNION",
    "SSCAN",
]


# Function to generate random data (string)
def generate_data():
    return f"data-{random.randint(1, 1000)}"


def generate_data_by_key(key):
    num = key.split("-")[1]  # Extract the number from the key
    return f"data-{num}"


def generate_data_from_key(key):
    prefix = key.split("-")[0]  # Extract the prefix from the key

    if prefix == "set":
        # Retrieve a random member from the specified set using srandmember()
        member = redis_client.srandmember(key)
        if member:
            return member.decode("utf-8")
        else:
            return None  # Return None if the set is empty or key does not exist
    elif prefix == "list":
        # Retrieve a member from the specified list using lindex
        index = random.randint(0, redis_client.llen(key) - 1)  # Generate a random index
        member = redis_client.lindex(key, index)
        if member:
            return member.decode("utf-8")
        else:
            return None  # Return None if the list is empty or key does not exist
    elif prefix == "zset":
        # Retrieve a random member from the specified zset using zrandmember()
        member = redis_client.zrandmember(key)
        if member:
            return member.decode("utf-8")
        else:
            return None  # Return None if the zset is empty or key does not exist
    elif prefix == "hash":
        # Retrieve a member from the specified hash using lindex
        member = redis_client.hrandfield(key)
        if member:
            return member.decode("utf-8")
        else:
            return None  # Return None if the hash is empty or key does not exist
    else:
        return None  # Return None for other prefixes or invalid keys


# Function to execute a random list command
def execute_command(command, key, other_key, dest_key):
    try:
        # list commands
        if command == "LPUSH":
            data = generate_data()
            logging.debug(f"LPUSH key='{key}' value='{data}': before")
            redis_client.lpush(key, data)
            logging.debug(f"LPUSH key='{key}' value='{data}': after")

        elif command == "LPOP":
            logging.debug(f"LPOP key='{key}'")
            elements = redis_client.lpop(key)
            if elements:
                for element in elements:
                    logging.debug(
                        f"LPOP key='{key}' element='{element.decode('utf-8')}'"
                    )
            else:
                logging.debug(f"LPOP key='{key}': list is empty")

        elif command == "RPOP":
            logging.debug(f"RPOP key='{key}'")
            elements = redis_client.rpop(key)
            if elements:
                for element in elements:
                    logging.debug(
                        f"RPOP key='{key}' element='{element.decode('utf-8')}'"
                    )
            else:
                logging.debug(f"RPOP key='{key}': list is empty")

        elif command == "LREM":
            value_to_remove = generate_data()
            count = random.randint(1, 4)  # Adjust max count based on list size
            logging.debug(f"LREM key='{key}' count={count} value='{value_to_remove}'")
            removed_count = redis_client.lrem(key, count, value_to_remove)
            logging.debug(
                f"LREM key='{key}' count={count} value='{value_to_remove}': {removed_count} elements removed"
            )

        elif command == "LTRIM":
            start = random.randint(0, 4)  # Adjust max start based on list size
            end = random.randint(start, 4)
            logging.debug(f"LTRIM key='{key}' start={start} end={end}: before")
            redis_client.ltrim(key, start, end)
            logging.debug(f"LTRIM key='{key}' start={start} end={end}: after")

        elif command == "LSET":
            if redis_client.llen(key) > 0:  # Ensure list is not empty
                index = random.randint(0, redis_client.llen(key) - 1)
                new_value = generate_data()
                logging.debug(
                    f"LSET key='{key}' index={index} value='{new_value}': before"
                )
                redis_client.lset(key, index, new_value)
                logging.debug(
                    f"LSET key='{key}' index={index} value='{new_value}': after"
                )
            else:
                logging.debug(f"LSET key='{key}': list is empty")

        elif command == "BLPOP":
            timeout = random.randint(1, 5)  # Set a timeout for blocking
            logging.debug(f"BLPOP key='{key}' timeout={timeout}")
            element = redis_client.blpop(key, timeout=timeout)
            if element:
                logging.debug(f"BLPOP key='{key}': {element[1].decode('utf-8')}")
            else:
                logging.debug(f"BLPOP key='{key}': timeout reached")

        elif command == "BRPOP":
            logging.debug(f"BRPOP key='{key}'")
            element = redis_client.brpop(key)
            if element:
                logging.debug(f"BRPOP key='{key}': {element[1].decode('utf-8')}")
            else:
                logging.debug(f"BRPOP key='{key}': list is empty")

        elif command == "BRPOPLPUSH":
            source_key = f"list-{random.randint(1, 10)}"  # Generate a random source key
            destination = (
                f"list-{random.randint(1, 10)}"  # Generate a random destination key
            )
            timeout = random.randint(1, 5)  # Set a timeout for blocking
            logging.debug(
                f"BRPOPLPUSH source='{source_key}' destination='{destination}' timeout={timeout}"
            )
            element = redis_client.brpoplpush(source_key, destination, timeout=timeout)
            if element:
                logging.debug(
                    f"BRPOPLPUSH source='{source_key}': {element.decode('utf-8')}"
                )
            else:
                logging.debug(f"BRPOPLPUSH source='{source_key}': timeout reached")

        elif command == "LINSERT":
            pivot = generate_data_from_key(key)
            where = random.choice(["before", "after"])  # Insert before or after
            value_to_insert = generate_data()
            logging.debug(
                f"LINSERT key='{key}' where='{where}' pivot='{pivot}' value='{value_to_insert}': before"
            )
            redis_client.linsert(key, where, pivot, value_to_insert)
            logging.debug(
                f"LINSERT key='{key}' where='{where}' pivot='{pivot}' value='{value_to_insert}': after"
            )

        elif command == "LMOVE":
            source_key = key
            logging.debug(f"LMOVE source='{source_key}' destination='{dest_key}'")
            element = redis_client.lmove(source_key, dest_key)
            if element:
                logging.debug(
                    f"LMOVE source='{source_key}' destination='{dest_key}': {element.decode('utf-8')}"
                )
            else:
                logging.debug(
                    f"LMOVE source='{source_key}' destination='{dest_key}': source list is empty"
                )

        elif command == "RPUSH":
            data = generate_data()
            logging.debug(f"RPUSH key='{key}' value='{data}': before")
            redis_client.rpush(key, data)
            logging.debug(f"RPUSH key='{key}' value='{data}': after")

        elif command == "LPUSHX":
            data = generate_data()
            logging.debug(f"LPUSHX key='{key}' value='{data}': before")
            redis_client.lpushx(key, data)
            logging.debug(f"LPUSHX key='{key}' value='{data}': after")

        elif command == "RPUSHX":
            data = generate_data()
            logging.debug(f"RPUSHX key='{key}' value='{data}': before")
            redis_client.rpushx(key, data)
            logging.debug(f"RPUSHX key='{key}' value='{data}': after")

        # set commands
        elif command == "SADD":
            # Add members to a Set
            data = generate_data()
            # data = generate_data_by_key(key)
            logging.debug(f"SADD key='{key}' value='{data}': before")
            redis_client.sadd(key, data)
            logging.debug(f"SADD key='{key}' value='{data}': after")

        elif command == "SREM":
            # Remove members from a Set
            data = generate_data()
            logging.debug(f"SREM key='{key}' value='{data}':")
            removed_count = redis_client.srem(key, data)
            logging.debug(
                f"SREM key='{key}' value='{data}': Removed {removed_count} member(s)"
            )

        elif command == "SPOP":
            # Remove and return a random member from a Set
            logging.debug(f"SPOP key='{key}':")
            member = redis_client.spop(key)
            if member:
                logging.debug(f"SPOP key='{key}': {member.decode('utf-8')}")
            else:
                logging.debug(f"SPOP key='{key}': Set is empty")

        elif command == "SDIFFSTORE":
            # Store the difference between Sets
            logging.debug(
                f"SDIFFSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}':"
            )
            stored_count = redis_client.sdiffstore(dest_key, key, other_key)
            logging.debug(
                f"SDIFFSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}': Stored {stored_count} member(s)"
            )

        elif command == "SINTERSTORE":
            # Store the intersection of Sets
            logging.debug(
                f"SINTERSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}':"
            )

            stored_count = redis_client.sinterstore(dest_key, key, other_key)
            logging.debug(
                f"SINTERSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}': Stored {stored_count} member(s)"
            )

        elif command == "SMOVE":
            # Move a member from one Set to another
            member = generate_data_from_key(key)
            if member:
                logging.debug(
                    f"SMOVE key='{key}' other_key='{other_key}' member='{member}':"
                )
                moved = redis_client.smove(key, other_key, member)
                if moved:
                    logging.debug(
                        f"SMOVE key='{key}' other_key='{other_key}' member='{member}': Moved member to another Set"
                    )
                else:
                    logging.debug(
                        f"SMOVE key='{key}' other_key='{other_key}' member='{member}': Member not found or already in the other Set"
                    )
            else:
                logging.debug(
                    f"SMOVE key='{key}' other_key='{other_key}': Key is empty"
                )

        elif command == "SUNIONSTORE":
            # Store the union of Sets
            logging.debug(
                f"SUNIONSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}':"
            )
            stored_count = redis_client.sunionstore(dest_key, key, other_key)
            logging.debug(
                f"SUNIONSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}': Stored {stored_count} member(s)"
            )

        # zset commands
        elif command == "ZADD":
            # Add members to a ZSET with scores
            members_with_scores = [
                (generate_data(), random.uniform(0, 10)) for _ in range(2)
            ]  # Generate sample members and scores
            member_score_dict = {member: score for member, score in members_with_scores}
            logging.debug(
                f"ZADD key='{key}' members_with_scores={members_with_scores}: before"
            )
            redis_client.zadd(key, member_score_dict)
            logging.debug(
                "ZADD key='{key}' members_with_scores={members_with_scores}: after"
            )

        elif command == "ZREM":
            # Remove members from a ZSET
            member = generate_data()
            logging.debug(f"ZREM key='{key}' member='{member}'")
            removed_count = redis_client.zrem(key, member)
            logging.debug(
                f"ZREM key='{key}' member='{member}': Removed {removed_count} member(s)"
            )

        elif command == "ZREMRANGE":
            # Remove all members in a sorted set within the given indexes
            start_index = random.randint(0, 10)
            end_index = random.randint(start_index, 20)
            logging.debug(f"ZREMRANGE key='{key}' start={start_index} end={end_index}")
            removed_count = redis_client.zremrangebyrank(key, start_index, end_index)
            logging.debug(
                f"ZREMRANGE key='{key}' start={start_index} end={end_index}: Removed {removed_count} member(s)"
            )

        elif command == "ZPOPMIN":
            # Remove and return members with the lowest scores from one or more sorted sets
            count = random.randint(1, 4)
            logging.debug(f"ZPOPMIN key='{key}' count={count}")
            members = redis_client.zpopmin(key, count)
            logging.debug(f"ZPOPMIN key='{key}' count={count}: {members}")

        elif command == "ZPOPMAX":
            # Remove and return members with the highest scores from one or more sorted sets
            count = random.randint(1, 4)
            logging.debug(f"ZPOPMAX key='{key}' count={count}")
            members = redis_client.zpopmax(key, count)
            logging.debug(f"ZPOPMAX key='{key}' count={count}: {members}")

        elif command == "ZUNIONSTORE":
            # Compute the union of multiple sorted sets and store the result in a new sorted set
            destination = dest_key
            keys = [
                key,
                other_key,
            ]
            logging.debug(f"ZUNIONSTORE destination='{destination}' keys={keys}")
            aggregate = "SUM"  # Example of aggregation function
            redis_client.zunionstore(destination, keys, aggregate=aggregate)
            logging.debug(
                f"ZUNIONSTORE destination='{destination}' keys={keys}: Union stored in '{destination}'"
            )

        elif command == "ZDIFFSTORE":
            # Store the difference between Sets
            logging.debug(
                f"ZDIFFSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}':"
            )
            stored_count = redis_client.zdiffstore(dest_key, key, other_key)
            logging.debug(
                f"ZDIFFSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}': Stored {stored_count} member(s)"
            )

        elif command == "ZINTERSTORE":
            # Store the intersection of Sets
            logging.debug(
                f"ZINTERSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}':"
            )
            aggregate = "SUM"  # Example of aggregation function
            stored_count = redis_client.zinterstore(
                dest_key, key, other_key, aggregate=aggregate
            )
            logging.debug(
                f"ZINTERSTORE dest_key='{dest_key}' key='{key}' other_key='{other_key}': Stored {stored_count} member(s)"
            )

        # hash commands
        elif command == "HSET":
            field_value_pairs = [
                (generate_data(), random.uniform(0, 10)) for _ in range(2)
            ]
            field_value_dict = {field: value for field, value in field_value_pairs}
            logging.debug(
                f"HSET key='{key}' field_value_pairs={field_value_pairs}: before"
            )
            redis_client.hset(name=key, mapping=field_value_dict)
            logging.debug(
                f"HSET key='{key}' field_value_pairs={field_value_pairs}: after"
            )

            field_value_pairs_int = [
                (generate_data(), random.randint(0, 10)) for _ in range(2)
            ]
            field_value_dict_int = {
                field: value for field, value in field_value_pairs_int
            }
            logging.debug(
                f"HSET key='{key}' field_value_pairs_int={field_value_pairs_int}: before"
            )
            redis_client.hset(name=key, mapping=field_value_dict_int)
            logging.debug(
                f"HSET key='{key}' field_value_pairs_int={field_value_pairs_int}: after"
            )

        elif command == "HMSET":
            field_value_pairs = [
                (generate_data(), random.uniform(0, 10)) for _ in range(2)
            ]
            field_value_dict = {field: value for field, value in field_value_pairs}
            logging.debug(
                f"HMSET key='{key}' field_value_pairs={field_value_pairs}: before"
            )
            redis_client.hmset(name=key, mapping=field_value_dict)
            logging.debug(
                f"HMSET key='{key}' field_value_pairs={field_value_pairs}: after"
            )

            field_value_pairs_int = [
                (generate_data(), random.randint(0, 10)) for _ in range(2)
            ]
            field_value_dict_int = {
                field: value for field, value in field_value_pairs_int
            }
            logging.debug(
                f"HMSET key='{key}' field_value_pairs_int={field_value_pairs_int}: before"
            )
            redis_client.hmset(name=key, mapping=field_value_dict_int)
            logging.debug(
                f"HMSET key='{key}' field_value_pairs_int={field_value_pairs_int}: after"
            )

        elif command == "HINCRBY":
            field = generate_data_from_key(key)
            if field:
                logging.debug(f"HINCRBY key='{key}' field='{field}' : before")
                value = redis_client.hget(key, field)
                logging.debug(f"HINCRBY key='{key}' field='{field}' : after")
                value = value.decode("utf-8")
                try:
                    # Convert the decoded value to an integer
                    value = int(value)
                    increment = random.randint(0, 9)
                    logging.debug(
                        f"HINCRBY key='{key}' field='{field}' increment='{increment}'"
                    )
                    redis_client.hincrby(key, field, increment)
                except ValueError:
                    # Handle non-integer value (conversion failed)
                    logging.debug(
                        f"HINCRBY key='{key}', field={field}, value={value} : value is not convertible to integer, value is of type:{type(value)}"
                    )
                else:
                    # This block executes only if conversion to int succeeds (optional)
                    logging.debug(
                        f"HINCRBY key='{key}' field='{field}': Successfully incremented integer value"
                    )
            else:
                logging.debug(f"HINCRBY key='{key}': Key is empty")

        elif command == "HINCRBYFLOAT":
            field = generate_data_from_key(key)
            if field:
                increment = random.random()
                logging.debug(
                    f"HINCRBYFLOAT key='{key}' field='{field}' increment='{increment}'"
                )
                redis_client.hincrbyfloat(key, field, increment)
            else:
                logging.debug(f"HINCRBYFLOAT key='{key}': Key is empty")

        elif command == "HDEL":
            fields = [generate_data() for _ in range(2)]  # Generate sample fields
            logging.debug(f"HDEL key='{key}' fields='{fields}'")
            redis_client.hdel(key, *fields)

        # string commands
        elif command == "SET":
            value = generate_data()  # Expecting a single argument (value)
            logging.debug(f"SET key='{key}' value='{value}': before")
            redis_client.set(key, value)
            logging.debug(f"SET key='{key}' value='{value}': after")

        elif command in ("SETNX", "GETSET", "APPEND"):
            value = generate_data()  # Obtain the value for these commands
            logging.debug(f"{command} key='{key}' value='{value}': before")
            if command == "SETNX":
                redis_client.setnx(key, value)
            elif command == "GETSET":
                redis_client.getset(key, value)
            else:  # command == "APPEND"
                redis_client.append(key, value)
            logging.debug(f"{command} key='{key}' value='{value}': after")

        elif command in ("MSET", "MSETNX"):
            value1 = generate_data()  # Generate first value
            value2 = generate_data()  # Generate second value
            data_dict = {
                key: value1,
                other_key: value2,
            }  # Create dictionary with key-value pairs
            logging.debug(f"{command} {data_dict}: before")
            if command == "MSET":
                redis_client.mset(data_dict)
            else:  # command == "MSETNX"
                redis_client.msetnx(data_dict)
            logging.debug(f"{command} {data_dict}: after")

        elif command in ("PSETEX", "SETEX"):
            expiry = random.randint(1, 10)
            value = generate_data()  # Obtain the value to set
            logging.debug(
                f"{command} key='{key}' {'milliseconds' if command == 'PSETEX' else 'seconds'}={expiry} value='{value}': before"
            )
            if command == "PSETEX":
                redis_client.psetex(key, expiry, value)
            else:  # command == "SETEX"
                redis_client.setex(key, expiry, value)
            logging.debug(
                f"{command} key='{key}' {'milliseconds' if command == 'PSETEX' else 'seconds'}={expiry} value='{value}': after"
            )

        elif command == "INCRBYFLOAT":
            value = random.random()
            logging.debug(f"{command} key='{key}' value={value}: before")
            redis_client.incrbyfloat(key, value)
            logging.debug(f"{command} key='{key}' value={value}: after")

        elif command in ("INCR", "DECR"):
            logging.debug(f"{command} key='{key}' value={1}: before")
            if command == "INCR":
                redis_client.incr(key)
            else:
                redis_client.decr(key)
            logging.debug(f"{command} key='{key}' value={1}: after")

        elif command in ("INCRBY", "DECRBY"):
            value = random.randint(1, 10)
            logging.debug(f"{command} key='{key}' value={value}: before")
            if command == "INCRBY":
                redis_client.incrby(key, value)
            else:
                redis_client.decrby(key, value)
            logging.debug(f"{command} key='{key}' value={value}: after")

        # temporarily disabled: could introduce decode error
        # elif command == "SETBIT":
        #     offset = random.randint(1, 10)
        #     value = random.randint(0, 1)
        #     logging.debug(f"SETBIT key='{key}' offset={offset} value={value}: before")
        #     redis_client.setbit(key, offset, value)
        #     logging.debug(f"SETBIT key='{key}' offset={offset} value={value}: after")

        elif command == "SETRANGE":
            offset = random.randint(1, 10)
            value = generate_data()
            logging.debug(
                f"SETRANGE key='{key}' offset={offset} value='{value}': before"
            )
            redis_client.setrange(key, offset, value)
            logging.debug(
                f"SETRANGE key='{key}' offset={offset} value='{value}': after"
            )

        # invalid commands
        else:
            logging.debug(f"Invalid command: {command}")

    except redis.RedisError as redis_error:
        logging.debug(f"Error executing Redis command: {redis_error}")
        # Handle the error appropriately, e.g., log it, retry, or raise a custom exception

    except Exception as general_error:
        logging.debug(f"Unexpected error: {general_error}")
        # Handle general errors as needed


# Function to scan all data
def scan_data(is_verify):
    # Define an empty dictionary to store key-value pairs
    database_snapshot = {}

    # Use SCAN to iterate through all keys (replace 0 with cursor from previous scan)
    cursor = 0
    while True:
        cursor, keys = redis_client.scan(cursor=cursor, match="*")
        for key in keys:
            key_str = key.decode("utf-8")  # Decode key from bytes to string

            # Check if key starts with "list-" (List) or "set-" (Set)
            if key_str.startswith("list-"):
                logging.debug(f"LRANGE key='{key_str}':")
                elements = redis_client.lrange(key_str, 0, -1)
                if elements:
                    # Decode elements within the list
                    elements = [
                        element.decode("utf-8") for element in elements
                    ]  # Decode elements

                    sorted_elements = sorted(elements)
                    database_snapshot[key_str] = sorted_elements

            elif key_str.startswith("set-"):
                members = redis_client.smembers(key_str)
                if members:
                    logging.debug(f"SMEMBERS key='{key_str}':")
                    # Decode members within the Set
                    members = [member.decode("utf-8") for member in members]

                    # Sort the members using Python's sorted() function
                    sorted_members = sorted(members)
                    database_snapshot[key_str] = sorted_members
                else:
                    logging.debug(f"SMEMBERS key='{key_str}': Set is empty")

            elif key_str.startswith("zset-"):  # Handle ZSET keys
                members_with_scores = redis_client.zrange(
                    key_str, 0, -1, withscores=True
                )  # Get all members and scores
                if members_with_scores:
                    logging.debug(f"ZRANGE key='{key_str}':")
                    # Decode members and scores
                    members_with_scores = [
                        (member.decode("utf-8"), score)
                        for member, score in members_with_scores
                    ]
                    database_snapshot[key_str] = members_with_scores
                else:
                    logging.debug(f"ZSET key='{key_str}' is empty")

            elif key_str.startswith("hash-"):  # Handle HASH keys
                field_value_pairs = redis_client.hgetall(
                    key_str
                )  # Get all fields and values
                if field_value_pairs:
                    logging.debug(f"HGETALL key='{key_str}':")
                    # Decode members and scores
                    field_value_pairs = [
                        (field.decode("utf-8"), value.decode("utf-8"))
                        for field, value in field_value_pairs.items()
                    ]

                    sorted_field_value_pairs = sorted(field_value_pairs)
                    database_snapshot[key_str] = sorted_field_value_pairs
                else:
                    logging.debug(f"HGETALL key='{key_str}' is empty")

            elif key_str.startswith("string-"):  # Handle STRING keys
                value = redis_client.get(key_str)  # Get all fields and values
                if value:
                    logging.debug(f"GET key='{key_str}':")
                    # Decode members and scores
                    value = value.decode("utf-8")
                    database_snapshot[key_str] = value
                else:
                    logging.debug(f"GET key='{key_str}' does not exist")

            else:
                logging.debug(f"Unknown key type for '{key_str}'")

        if cursor == 0:
            break

    if is_verify:
        snapshot_name = "database_snapshot_after_replay.json"
    else:
        snapshot_name = "database_snapshot_before_replay.json"

    # Output the snapshot data as JSON (modify for your desired format)
    with open(snapshot_name, "w") as f:
        json.dump(database_snapshot, f, indent=4)

    logging.debug(f"Database snapshot exported to: {snapshot_name} ")
    return f"data-{random.randint(1, 1000)}"


# TODO(ZX) replay lua script
def lua_script_test(command, key, other_key, dest_key):
    lua = ""
    if command == "LINSERT":
        lua = """
            local pivot = ARGV[2]
            local value = ARGV[3]
            redis.call('LINSERT', KEYS[1], ARGV[1], pivot, value)
            return
        """
    elif command == "LPOP":
        lua = """
            local value = redis.call('LPOP', KEYS[1])
            return value
        """
    elif command == "LPUSH":
        lua = """
            local value = ARGV[2]
            redis.call('LPUSH', KEYS[1], value)
            return
        """
    elif command == "LPUSHX":
        lua = """
            local value = ARGV[2]
            redis.call('LPUSHX', KEYS[1], value)
            return
        """
    elif command == "LREM":
        lua = """
            local count = tonumber(ARGV[2])
            local value = ARGV[3]
            redis.call('LREM', KEYS[1], count, value)
            return
        """
    elif command == "RPUSH":
        lua = """
            local value = ARGV[2]
            redis.call('RPUSH', KEYS[1], value)
            return
        """
    elif command == "RPUSHX":
        lua = """
            local value = ARGV[2]
            redis.call('RPUSHX', KEYS[1], value)
            return
        """
    elif command == "LSET":
        lua = """
            local index = tonumber(ARGV[2])
            local value = ARGV[3]
            redis.call('LSET', KEYS[1], index, value)
            return
        """
    elif command == "RPOP":
        lua = """
            local value = redis.call('RPOP', KEYS[1])
            return value
        """
    elif command == "LTRIM":
        lua = """
            local start = tonumber(ARGV[2])
            local stop = tonumber(ARGV[3])
            redis.call('LTRIM', KEYS[1], start, stop)
            return
        """
    elif command == "SET":
        lua = """
            local value = ARGV[2]
            redis.call('SET', KEYS[1], value)
            return
        """
    elif command == "SETNX":
        lua = """
            local value = ARGV[2]
            redis.call('SETNX', KEYS[1], value)
            return
        """
    elif command == "GETSET":
        lua = """
            local value = ARGV[2]
            local old_value = redis.call('GETSET', KEYS[1], value)
            return old_value
        """
    elif command == "PSETEX":
        lua = """
            local ttl = tonumber(ARGV[2])
            local value = ARGV[3]
            redis.call('PSETEX', KEYS[1], ttl, value)
            return
        """
    elif command == "SETEX":
        lua = """
            local ttl = tonumber(ARGV[2])
            local value = ARGV[3]
            redis.call('SETEX', KEYS[1], ttl, value)
            return
        """
    elif command == "INCRBYFLOAT":
        lua = """
            local increment = tonumber(ARGV[2])
            redis.call('INCRBYFLOAT', KEYS[1], increment)
            return
        """
    elif command == "SETBIT":
        lua = """
            local offset = tonumber(ARGV[2])
            local value = tonumber(ARGV[3])
            redis.call('SETBIT', KEYS[1], offset, value)
            return
        """
    elif command == "SETRANGE":
        lua = """
            local offset = tonumber(ARGV[2])
            local value = ARGV[3]
            redis.call('SETRANGE', KEYS[1], offset, value)
            return
        """
    elif command == "APPEND":
        lua = """
            local value = ARGV[2]
            redis.call('APPEND', KEYS[1], value)
            return
        """
    elif command == "INCR":
        lua = """
            redis.call('INCR', KEYS[1])
            return
        """
    elif command == "INCRBY":
        lua = """
            local increment = tonumber(ARGV[2])
            redis.call('INCRBY', KEYS[1], increment)
            return
        """
    elif command == "DECR":
        lua = """
            redis.call('DECR', KEYS[1])
            return
        """
    elif command == "DECRBY":
        lua = """
            local decrement = tonumber(ARGV[2])
            redis.call('DECRBY', KEYS[1], decrement)
            return
        """
    elif command == "HSET":
        lua = """
            local field = ARGV[2]
            local value = ARGV[3]
            redis.call('HSET', KEYS[1], field, value)
            return
        """
    elif command == "HINCRBY":
        lua = """
            local field = ARGV[2]
            local increment = tonumber(ARGV[3])
            redis.call('HINCRBY', KEYS[1], field, increment)
            return
        """
    elif command == "HINCRBYFLOAT":
        lua = """
            local field = ARGV[2]
            local increment = tonumber(ARGV[3])
            redis.call('HINCRBYFLOAT', KEYS[1], field, increment)
            return
        """
    elif command == "HDEL":
        lua = """
            local field = ARGV[2]
            redis.call('HDEL', KEYS[1], field)
            return
        """
    elif command == "HMSET":
        lua = """
            for i = 2, #ARGV, 2 do
                local field = ARGV[i]
                local value = ARGV[i + 1]
                redis.call('HMSET', KEYS[1], field, value)
            end
            return
        """
    elif command == "ZADD":
        lua = """
            local score = tonumber(ARGV[2])
            local member = ARGV[3]
            redis.call('ZADD', KEYS[1], score, member)
            return
        """
    elif command == "ZREM":
        lua = """
            local member = ARGV[2]
            redis.call('ZREM', KEYS[1], member)
            return
        """
    elif command == "ZREMRANGE":
        lua = """
            local start = tonumber(ARGV[2])
            local stop = tonumber(ARGV[3])
            redis.call('ZREMRANGE', KEYS[1], start, stop)
            return
        """
    elif command == "ZPOPMIN":
        lua = """
            local count = tonumber(ARGV[2])
            redis.call('ZPOPMIN', KEYS[1], count)
            return
        """
    elif command == "ZPOPMAX":
        lua = """
            local count = tonumber(ARGV[2])
            redis.call('ZPOPMAX', KEYS[1], count)
            return
        """
    elif command == "SADD":
        lua = """
            local value = ARGV[2]
            redis.call('SADD', KEYS[1], value)
            return
        """
    elif command == "SPOP":
        lua = """
            redis.call('SPOP', KEYS[1])
            return
        """
    elif command == "SREM":
        lua = """
            local value = ARGV[2]
            redis.call('SREM', KEYS[1], value)
            return
        """
    else:
        lua = """
            return
        """

    execute_script = redis_client.register_script(lua)
    value = generate_data()  # Expecting a single argument (value)

    if command in [
        "LINSERT",
        "LREM",
        "LSET",
        "LTRIM",
        "PSETEX",
        "SETEX",
        "HSET",
        "HINCRBY",
        "HINCRBYFLOAT",
        "HDEL",
        "HMSET",
        "ZADD",
        "ZREM",
        "ZREMRANGE",
        "ZPOPMIN",
        "ZPOPMAX",
    ]:
        execute_script(keys=[key], args=[command, other_key, dest_key])
    elif command in ["SETBIT", "SETRANGE", "INCRBY", "DECRBY", "INCRBYFLOAT"]:
        execute_script(keys=[key], args=[command, other_key, dest_key])
    else:
        execute_script(keys=[key], args=[command, value])

    redis_client.get(key)


def generate_single_obj_cmd(i, obj_cnt):
    key_prefix = random.choice(["set-", "zset-"])
    key = f"{key_prefix}{random.randint(1, obj_cnt)}"
    other_key = f"{key_prefix}{random.randint(1, obj_cnt)}"
    dest_key = f"{key_prefix}{random.randint(1, obj_cnt)}"

    if key_prefix == "list-":
        if i == 0:
            command = "RPOP"
        elif i == 1:
            command = "LPUSH"
        elif random.random() < 0.3:  # Adjust this probability as needed
            command = "LPUSH"
        else:
            command = random.choice(list_modifiable_commands)
    elif key_prefix == "set-":
        if i == 0:
            command = "SPOP"
        elif i == 1:
            command = "SADD"
        elif random.random() < 0.3:  # Adjust this probability as needed
            command = "SADD"
        else:
            command = random.choice(set_modifiable_commands)
    elif key_prefix == "zset-":
        if i == 0:
            command = "ZPOPMAX"
        elif i == 1:
            command = "ZADD"
        elif random.random() < 0.3:  # Adjust this probability as needed
            command = "ZADD"
        else:
            command = random.choice(zset_modifiable_commands)
    elif key_prefix == "hash-":
        if i == 0:
            command = "HDEL"
        elif i == 1:
            command = "HSET"
        else:
            command = random.choice(hash_modifiable_commands)
    elif key_prefix == "string-":
        if i == 0:
            command = "INCR"
        elif i == 1:
            command = "SET"
        else:
            command = random.choice(string_modifiable_commands)
    else:
        print(f"Invalid data type: {key_prefix}")
        assert False

    execute_command(command, key, other_key, dest_key)


def generate_multi_obj_cmd(i, obj_cnt):
    # options: ["list-", "set-", "zset-", "string-"]
    key_prefix = random.choice(["set-", "zset-"])
    key = f"{key_prefix}{random.randint(1, obj_cnt)}"
    other_key = f"{key_prefix}{random.randint(1, obj_cnt)}"
    dest_key = f"{key_prefix}{random.randint(obj_cnt+1, obj_cnt*2)}"

    if key_prefix == "list-":
        command = random.choice(list_modifiable_commands_multi_object)
    elif key_prefix == "set-":
        command = random.choice(set_modifiable_commands_multi_object)
    elif key_prefix == "zset-":
        command = random.choice(zset_modifiable_commands_multi_object)
    elif key_prefix == "string-":
        command = random.choice(string_modifiable_commands_multi_object)
    else:
        print(f"Invalid data type: {key_prefix}")
        assert False

    execute_command(command, key, other_key, dest_key)


# TODO(ZX) replay multi commands
if __name__ == "__main__":
    # Configuration for connecting to your Redis instance
    # redis_client = redis.Redis(host="localhost", port=6379)

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--port", type=int, default=6379, help="test server port"
    )
    parser.add_argument(
        "--verify", action="store_true", help="Execute verification code"
    )
    parser.add_argument("--load", action="store_true", help="Execute loading code")
    parser.add_argument("--lua", action="store_true", help="Execute lua code")
    args = parser.parse_args()

    redis_client = redis.Redis(host="localhost", port=args.port)

    print("port:",args.port)
    if args.verify:
        # Code to be executed only when the --verify flag is present
        print("Verify log replay")

        scan_data(args.verify)

    elif args.load:
        # Code to be executed normally
        print("Generate data and log")

        redis_client.flushdb()

        obj_cnt = 5

        # Use ThreadPoolExecutor to manage the threads
        with ThreadPoolExecutor(max_workers=10) as executor:
            for i in range(200):
                executor.submit(generate_single_obj_cmd, i, obj_cnt)

        # increase max_workers for multi_obj may introduce deadlock
        with ThreadPoolExecutor(max_workers=1) as executor:
            for i in range(20):
                executor.submit(generate_multi_obj_cmd, i, obj_cnt)

        print("done loading data")
        scan_data(args.verify)

    elif args.lua:
        print("Execute Lua script")

        redis_client.flushdb()

        # Execute multi-object commands.
        obj_cnt = 5
        for i in range(10):
            # options: ["list-", "set-", "zset-", "hash-", "string-"]
            key_prefix = random.choice(["string-"])
            key = f"{key_prefix}{random.randint(1, obj_cnt)}"
            other_key = f"{key_prefix}{random.randint(1, obj_cnt)}"
            dest_key = f"{key_prefix}{random.randint(obj_cnt+1, obj_cnt*2)}"

            if key_prefix == "list-":
                command = random.choice(list_modifiable_commands)
            elif key_prefix == "set-":
                command = random.choice(set_modifiable_commands)
            elif key_prefix == "zset-":
                command = random.choice(zset_modifiable_commands)
            elif key_prefix == "string-":
                command = random.choice(string_modifiable_commands)
            else:
                print(f"Invalid data type: {key_prefix}")
                assert False

            # serial execution
            lua_script_test(command, key, other_key, dest_key)

        print("done loading lua script data")
        scan_data(args.verify)

    else:
        assert False
